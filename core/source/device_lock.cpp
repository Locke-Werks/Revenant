#include "core/source/device_lock.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <sddl.h>

#endif  // _WIN32

namespace revenant::source {
namespace {

// What the holder thread found when it asked for the mutex.
struct Attempt {
    enum class Result : std::uint8_t { Acquired, Abandoned, TimedOut, Failed };
    Result result = Result::Failed;
    std::string failure;
    long long code = 0;
};

#ifdef _WIN32

[[nodiscard]] std::string win32_message(DWORD code)
{
    LPSTR buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        if (buffer != nullptr) {
            LocalFree(buffer);
        }
        return std::format("Windows error {}", code);
    }
    std::string text(buffer, length);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

[[nodiscard]] std::wstring widen(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(needed, 0)), L'\0');
    if (needed > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                            needed);
    }
    return out;
}

// WHO MAY USE THE MUTEX. The first process to create it sets its security for
// everyone after, and the processes that need it run as different accounts:
// the CI runner as NETWORK SERVICE, the owner's engine and tools as the owner.
// A default descriptor grants the creator and administrators only, so whichever
// of the two came second would be refused with ERROR_ACCESS_DENIED and the lock
// would stop serialising exactly the pair it exists for.
//
// So: full control for SYSTEM and administrators, and for everyone exactly the
// two rights a holder uses, SYNCHRONIZE to wait and MUTEX_MODIFY_STATE, which is
// 0x0001, to release. Nothing that lets another account change the descriptor
// or take ownership of the object. The mandatory label is low with no-write-up,
// because a service runs at system integrity and a desktop process at medium,
// and without a label the object would take its creator's level and a lower
// process could be refused the release.
//
// Nothing here is a secret. The worst another account can do with these
// rights is hold the lock, which stops Revenant opening a dongle that account
// could equally well hold by opening it.
constexpr wchar_t kMutexSecurity[] =
    L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;WD)S:(ML;;NW;;;LW)";

// Runs on the holder thread for the whole time the lock is held.
void hold(const std::string& name, DWORD wait_ms, std::promise<Attempt>& ready,
          std::future<void>& release)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kMutexSecurity, SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
        const DWORD error = GetLastError();
        ready.set_value(Attempt{Attempt::Result::Failed,
                                std::format("the security descriptor for {} could not be built: {}",
                                            name, win32_message(error)),
                                static_cast<long long>(error)});
        return;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    const std::wstring wide = widen(name);
    HANDLE mutex =
        CreateMutexExW(&attributes, wide.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    const DWORD create_error = GetLastError();
    LocalFree(descriptor);

    if (mutex == nullptr) {
        ready.set_value(Attempt{
            Attempt::Result::Failed,
            std::format("the lock {} could not be opened: CreateMutexExW failed with {}", name,
                        win32_message(create_error)),
            static_cast<long long>(create_error)});
        return;
    }

    const DWORD waited = WaitForSingleObject(mutex, wait_ms);
    switch (waited) {
        case WAIT_OBJECT_0:
        case WAIT_ABANDONED: {
            ready.set_value(Attempt{waited == WAIT_ABANDONED ? Attempt::Result::Abandoned
                                                             : Attempt::Result::Acquired,
                                    {},
                                    0});
            release.wait();
            ReleaseMutex(mutex);
            CloseHandle(mutex);
            return;
        }
        case WAIT_TIMEOUT: {
            CloseHandle(mutex);
            ready.set_value(Attempt{Attempt::Result::TimedOut, {}, kDeviceLockHeldElsewhere});
            return;
        }
        default: {
            const DWORD error = GetLastError();
            CloseHandle(mutex);
            ready.set_value(Attempt{
                Attempt::Result::Failed,
                std::format("waiting on the lock {} failed: {}", name, win32_message(error)),
                static_cast<long long>(error)});
            return;
        }
    }
}

[[nodiscard]] DWORD wait_ms_of(std::chrono::milliseconds wait)
{
    // INFINITE is 0xFFFFFFFF, so the largest finite wait is one less. Nobody
    // asks for 49 days; the clamp is so an arithmetic mistake cannot become an
    // unbounded wait.
    constexpr auto kLongest = static_cast<long long>(INFINITE) - 1;
    return static_cast<DWORD>(std::clamp<long long>(wait.count(), 0, kLongest));
}

#endif  // _WIN32

// The thread that owns one name's mutex, and the handshake with it.
class Holder {
public:
    Holder() = default;
    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;

    ~Holder()
    {
        release_.set_value();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Starts the thread and waits for it to say whether it got the mutex.
    [[nodiscard]] static Expected<std::unique_ptr<Holder>> start(const std::string& name,
                                                                 std::chrono::milliseconds wait)
    {
        auto holder = std::make_unique<Holder>();
        std::promise<Attempt> ready;
        std::future<Attempt> answer = ready.get_future();

#ifdef _WIN32
        try {
            holder->thread_ =
                std::thread([name, wait_ms = wait_ms_of(wait), promise = std::move(ready),
                             released = holder->release_.get_future()]() mutable {
                    hold(name, wait_ms, promise, released);
                });
        } catch (const std::system_error& error) {
            return fail(std::format("no thread could be started to hold the lock {}: {}", name,
                                    error.what()));
        }
#else
        // NOT ARBITRATED BETWEEN PROCESSES ON THIS PLATFORM. There is no dongle
        // backend tested off Windows yet, and the named mutex this stands in
        // for has no exact POSIX twin: flock on a file in a well-known place is
        // the likely one. Until then the lock is shared within the process and
        // granted at once, so nothing here refuses an open that would work.
        static_cast<void>(wait);
        ready.set_value(Attempt{Attempt::Result::Acquired, {}, 0});
#endif

        const Attempt attempt = answer.get();
        switch (attempt.result) {
            case Attempt::Result::Acquired:
                return holder;
            case Attempt::Result::Abandoned:
                holder->abandoned_ = true;
                return holder;
            case Attempt::Result::TimedOut:
                return fail(std::format("the lock {} is held by another process and was not "
                                        "released within {} ms",
                                        name, wait.count()),
                            kDeviceLockHeldElsewhere);
            case Attempt::Result::Failed:
                return fail(attempt.failure, attempt.code);
        }
        return fail(std::format("the lock {} answered with an outcome this build does not know",
                                name));
    }

    [[nodiscard]] bool abandoned() const { return abandoned_; }

private:
    std::promise<void> release_{};
    std::thread thread_{};
    bool abandoned_ = false;
};

// Every name this process holds or is acquiring, and how many holders share it.
struct Entry {
    int users = 0;
    bool acquiring = false;
    std::unique_ptr<Holder> holder{};
};

struct Registry {
    std::mutex lock;
    std::condition_variable changed;
    std::map<std::string, Entry, std::less<>> entries;
};

// LEAKED ON PURPOSE. A DeviceLock can be destroyed during static destruction,
// by a source some other static owns, and a registry that had already been
// destroyed would be released into freed memory. Left alive, a holder thread
// still running at exit dies with the process, and Windows releases its mutex
// as abandoned, which the next process counts as acquired. That is the same
// outcome as a crash and the design already handles it.
[[nodiscard]] Registry& registry()
{
    static Registry* const instance = new Registry;
    return *instance;
}

}  // namespace

Expected<DeviceLock> DeviceLock::acquire(std::string_view name, std::chrono::milliseconds wait)
{
    if (name.empty()) {
        return fail("a device lock needs a name");
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::max(wait, std::chrono::milliseconds(0));
    Registry& reg = registry();
    std::unique_lock guard(reg.lock);

    for (;;) {
        const auto found = reg.entries.find(name);
        if (found == reg.entries.end()) {
            break;
        }
        if (found->second.users > 0) {
            ++found->second.users;
            return DeviceLock(std::string(name), found->second.holder->abandoned());
        }

        // Another thread of this process is waiting for the same name. Waiting
        // behind it within this caller's own deadline means a probe that asked
        // not to wait does not wait, and an open that got the lock is shared
        // rather than contended.
        const bool settled = reg.changed.wait_until(guard, deadline, [&reg, name] {
            const auto again = reg.entries.find(name);
            return again == reg.entries.end() || !again->second.acquiring;
        });
        if (!settled) {
            return fail(std::format("the lock {} is held by another process and was not "
                                    "released within {} ms",
                                    name, wait.count()),
                        kDeviceLockHeldElsewhere);
        }
    }

    reg.entries[std::string(name)].acquiring = true;
    guard.unlock();

    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    auto holder = Holder::start(std::string(name), std::max(left, std::chrono::milliseconds(0)));

    guard.lock();
    const auto entry = reg.entries.find(name);
    entry->second.acquiring = false;
    reg.changed.notify_all();

    if (!holder) {
        if (entry->second.users == 0) {
            reg.entries.erase(entry);
        }
        return std::unexpected(std::move(holder.error()));
    }

    const bool abandoned = (*holder)->abandoned();
    entry->second.holder = std::move(*holder);
    entry->second.users = 1;
    return DeviceLock(std::string(name), abandoned);
}

void DeviceLock::release()
{
    if (name_.empty()) {
        return;
    }

    Registry& reg = registry();
    std::scoped_lock guard(reg.lock);
    const auto found = reg.entries.find(name_);
    if (found != reg.entries.end() && found->second.users > 0 && --found->second.users == 0) {
        // Joined with the registry held, so another thread of this process
        // cannot find the name free here while the mutex is still owned and
        // then time out against its own process. The holder thread takes no
        // lock of ours and only has ReleaseMutex left to do.
        found->second.holder.reset();
        reg.entries.erase(found);
    }
    name_.clear();
    abandoned_ = false;
}

bool held_by_this_process(std::string_view name)
{
    Registry& reg = registry();
    std::scoped_lock guard(reg.lock);
    const auto found = reg.entries.find(name);
    return found != reg.entries.end() && found->second.users > 0;
}

Expected<DeviceLock> lock_for_open(const DeviceLockPolicy& policy)
{
    auto lock = DeviceLock::acquire(policy.name, policy.open_wait);
    if (lock) {
        return lock;
    }
    if (!held_elsewhere(lock.error())) {
        return std::unexpected(with_context(std::move(lock.error()),
                                            std::format("taking the lock on {}", policy.device)));
    }
    return fail(std::format(
                    "another process holds {}: the machine-wide lock {} was still held after "
                    "waiting {} ms. Every Revenant process takes it before opening the device and "
                    "keeps it until the device is closed, so an engine, a revenant-cli or a test "
                    "run elsewhere on this machine is using it now.",
                    policy.device, policy.name, policy.open_wait.count()),
                kDeviceLockHeldElsewhere);
}

Expected<DeviceLock> lock_for_probe(const DeviceLockPolicy& policy)
{
    auto lock = DeviceLock::acquire(policy.name, std::chrono::milliseconds(0));
    if (lock) {
        return lock;
    }
    if (!held_elsewhere(lock.error())) {
        return std::unexpected(with_context(std::move(lock.error()),
                                            std::format("taking the lock on {}", policy.device)));
    }
    return fail(std::format("{}: the machine-wide lock {} is held, so {} was not opened to be "
                            "described",
                            kProbeHeldElsewhere, policy.name, policy.device),
                kDeviceLockHeldElsewhere);
}

}  // namespace revenant::source
