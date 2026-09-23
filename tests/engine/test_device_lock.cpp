// The machine-wide device lock, core/source/device_lock.h.
//
// EVERY CASE HERE CONTENDS WITH A SECOND PROCESS, revenant_lock_holder, and
// never with a second thread alone. The lock arbitrates between processes and
// deliberately shares one acquisition among the threads of a process, so a
// contender in the test's own process would prove nothing about the property
// the lock exists for.
//
// NONE OF THIS TOUCHES A RADIO. Each case takes a stand-in name of its own under
// Global\, so it exercises the same namespace and the same security descriptor
// the RTL-SDR lock uses without ever contending with an engine that is really
// streaming, and the open under test is a double that counts its calls. The
// wiring into rtlsdr_source.cpp is covered by the dongle cases in
// test_rtlsdr_source.cpp, which need the device.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <future>
#include <string>
#include <thread>
#include <utility>

#include "core/error.h"
#include "core/source/device_lock.h"
#include "core/source/rtlsdr_lock.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace revenant;
using namespace std::chrono_literals;

namespace {

// A fresh name per case and per run, so two checkouts running this suite at
// once never contend with each other and a case never inherits a holder a
// previous one left behind.
[[nodiscard]] std::string stand_in_name()
{
    static std::atomic<int> counter{0};
    return std::format("Global\\Revenant.LockTest.{}.{}", GetCurrentProcessId(),
                       counter.fetch_add(1) + 1);
}

[[nodiscard]] std::chrono::milliseconds since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start);
}

// revenant_lock_holder in a process of its own, with a pipe each way. Created
// with CREATE_NO_WINDOW, so nothing appears on the desktop of whoever is
// running the suite.
class Holder {
public:
    Holder(const std::string& name, const std::string& mode, const std::string& extra = {})
    {
        SECURITY_ATTRIBUTES inherit{};
        inherit.nLength = sizeof(inherit);
        inherit.bInheritHandle = TRUE;

        HANDLE child_out = nullptr;
        HANDLE child_in = nullptr;
        REQUIRE(CreatePipe(&out_, &child_out, &inherit, 0));
        REQUIRE(CreatePipe(&child_in, &in_, &inherit, 0));
        // The test's own ends stay out of the child, or the child would hold
        // its own stdin open and never see it close.
        SetHandleInformation(out_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(in_, HANDLE_FLAG_INHERIT, 0);

        const std::wstring exe =
            std::filesystem::path(REVENANT_LOCK_HOLDER_PATH).make_preferred().wstring();
        std::wstring command = L"\"" + exe + L"\" \"" +
                               std::filesystem::path(name).wstring() + L"\" " +
                               std::filesystem::path(mode).wstring();
        if (!extra.empty()) {
            command += L" " + std::filesystem::path(extra).wstring();
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_in;
        startup.hStdOutput = child_out;
        startup.hStdError = child_out;

        PROCESS_INFORMATION info{};
        const BOOL created = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
        const DWORD error = GetLastError();
        CloseHandle(child_out);
        CloseHandle(child_in);
        INFO("CreateProcessW failed with " << error << " for " << REVENANT_LOCK_HOLDER_PATH);
        REQUIRE(created);
        CloseHandle(info.hThread);
        process_ = info.hProcess;
    }

    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;

    ~Holder()
    {
        close_stdin();
        if (process_ != nullptr) {
            if (WaitForSingleObject(process_, 5'000) != WAIT_OBJECT_0) {
                TerminateProcess(process_, 1);
                WaitForSingleObject(process_, 5'000);
            }
            CloseHandle(process_);
        }
        if (out_ != nullptr) {
            CloseHandle(out_);
        }
    }

    // One line of the child's output, or empty when none arrived in time or
    // the child exited without writing one.
    [[nodiscard]] std::string read_line(std::chrono::milliseconds timeout = 10s)
    {
        std::string line;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD available = 0;
            if (!PeekNamedPipe(out_, nullptr, 0, nullptr, &available, nullptr)) {
                break;
            }
            if (available == 0) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            char c = 0;
            DWORD got = 0;
            if (!ReadFile(out_, &c, 1, &got, nullptr) || got == 0) {
                break;
            }
            if (c == '\n') {
                return line;
            }
            if (c != '\r') {
                line.push_back(c);
            }
        }
        return line;
    }

    // Tells a holding child to release and exit.
    void let_go() { close_stdin(); }

    // Ends the child without letting it release, which is what an engine that
    // crashed mid-stream looks like to the lock.
    void kill()
    {
        TerminateProcess(process_, 9);
        REQUIRE(WaitForSingleObject(process_, 10'000) == WAIT_OBJECT_0);
    }

    [[nodiscard]] DWORD exit_code(std::chrono::milliseconds timeout = 10s)
    {
        REQUIRE(WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) ==
                WAIT_OBJECT_0);
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        return code;
    }

private:
    void close_stdin()
    {
        if (in_ != nullptr) {
            CloseHandle(in_);
            in_ = nullptr;
        }
    }

    HANDLE process_ = nullptr;
    HANDLE out_ = nullptr;
    HANDLE in_ = nullptr;
};

// What another process sees of `name` right now.
[[nodiscard]] std::string seen_from_outside(const std::string& name)
{
    Holder probe(name, "try");
    return probe.read_line();
}

// A stand-in for a device handle. Records, when it closes, whether the lock
// that licensed it was still held at that moment.
struct FakeHandle {
    std::string lock_name;
    int* closes = nullptr;
    bool* lock_held_at_close = nullptr;

    FakeHandle(std::string name, int* close_count, bool* held_at_close)
        : lock_name(std::move(name)), closes(close_count), lock_held_at_close(held_at_close)
    {
    }

    FakeHandle(FakeHandle&& other) noexcept
        : lock_name(std::move(other.lock_name)),
          closes(std::exchange(other.closes, nullptr)),
          lock_held_at_close(std::exchange(other.lock_held_at_close, nullptr))
    {
    }

    FakeHandle& operator=(FakeHandle&&) = delete;
    FakeHandle(const FakeHandle&) = delete;
    FakeHandle& operator=(const FakeHandle&) = delete;

    ~FakeHandle()
    {
        if (closes != nullptr) {
            ++*closes;
            *lock_held_at_close = source::held_by_this_process(lock_name);
        }
    }
};

}  // namespace

TEST_CASE("a device lock is shared within a process and let go when its last holder goes",
          "[source][lock]") {
    const std::string name = stand_in_name();

    auto first = source::DeviceLock::acquire(name, 0ms);
    INFO((first ? std::string("held") : first.error().message));
    REQUIRE(first.has_value());
    CHECK(first->held());
    CHECK(first->name() == name);
    CHECK_FALSE(first->recovered_from_abandoned());
    CHECK(source::held_by_this_process(name));

    // A second holder in the same process shares the acquisition at once,
    // which is what lets one engine describe a dongle it is not streaming from
    // while it streams from another.
    const auto started = std::chrono::steady_clock::now();
    auto second = source::DeviceLock::acquire(name, 0ms);
    REQUIRE(second.has_value());
    CHECK(since(started) < 1s);

    CHECK(seen_from_outside(name) == "busy");

    first->release();
    CHECK_FALSE(first->held());
    CHECK(source::held_by_this_process(name));
    CHECK(seen_from_outside(name) == "busy");

    // Released on destruction, not only by release().
    {
        auto moved = std::move(*second);
        CHECK(moved.held());
        CHECK_FALSE(second->held());
    }
    CHECK_FALSE(source::held_by_this_process(name));
    CHECK(seen_from_outside(name) == "free");
}

TEST_CASE("a device lock held by another process makes a waiter time out at its bound",
          "[source][lock]") {
    const std::string name = stand_in_name();
    Holder other(name, "hold");
    REQUIRE(other.read_line() == "held");

    constexpr auto kWait = 400ms;
    const auto started = std::chrono::steady_clock::now();
    auto waited = source::DeviceLock::acquire(name, kWait);
    const auto elapsed = since(started);
    REQUIRE_FALSE(waited.has_value());
    INFO(waited.error().message << " after " << elapsed.count() << " ms");
    CHECK(source::held_elsewhere(waited.error()));
    CHECK(waited.error().message.find(name) != std::string::npos);
    // It waited, and it stopped. The upper bound is loose because ten builds
    // can share this machine; what it catches is a wait that never ends.
    CHECK(elapsed >= kWait - 50ms);
    CHECK(elapsed < 10s);

    // A zero wait asks once and returns.
    const auto asked = std::chrono::steady_clock::now();
    auto once = source::DeviceLock::acquire(name, 0ms);
    REQUIRE_FALSE(once.has_value());
    CHECK(source::held_elsewhere(once.error()));
    CHECK(since(asked) < 2s);

    CHECK_FALSE(source::held_by_this_process(name));

    other.let_go();
    CHECK(other.read_line() == "released");
    CHECK(other.exit_code() == 0u);

    auto after = source::DeviceLock::acquire(name, 2s);
    INFO((after ? std::string("held") : after.error().message));
    REQUIRE(after.has_value());
    CHECK_FALSE(after->recovered_from_abandoned());
}

TEST_CASE("a waiter gets the device lock as soon as the other process lets go",
          "[source][lock]") {
    const std::string name = stand_in_name();
    Holder other(name, "hold");
    REQUIRE(other.read_line() == "held");

    const auto started = std::chrono::steady_clock::now();
    auto waiting = std::async(std::launch::async,
                              [&name] { return source::DeviceLock::acquire(name, 20s); });

    std::this_thread::sleep_for(300ms);
    CHECK(waiting.wait_for(0ms) == std::future_status::timeout);
    other.let_go();

    auto got = waiting.get();
    const auto elapsed = since(started);
    INFO((got ? std::string("held") : got.error().message) << " after " << elapsed.count()
                                                           << " ms");
    REQUIRE(got.has_value());
    CHECK(elapsed >= 250ms);
    CHECK(elapsed < 15s);
    CHECK_FALSE(got->recovered_from_abandoned());
}

// Keeps the named mutex in existence without owning it, the way a process
// already waiting on it does. A named kernel object lives exactly as long as
// somebody has a handle to it.
class Onlooker {
public:
    explicit Onlooker(const std::string& name)
        : handle_(OpenMutexW(SYNCHRONIZE, FALSE, std::filesystem::path(name).wstring().c_str()))
    {
        INFO("OpenMutexW failed with " << GetLastError());
        REQUIRE(handle_ != nullptr);
    }
    Onlooker(const Onlooker&) = delete;
    Onlooker& operator=(const Onlooker&) = delete;
    ~Onlooker() { CloseHandle(handle_); }

private:
    HANDLE handle_ = nullptr;
};

// AN ENGINE THAT CRASHES WHILE STREAMING, WHICH HAS TWO OUTCOMES AND BOTH HAVE
// TO END WITH THE NEXT PROCESS HOLDING THE LOCK.
//
// With another process already waiting, or otherwise holding a handle, the
// mutex outlives its dead owner and Windows hands it to the next waiter with
// WAIT_ABANDONED, which has to count as acquired or a crash would lock the
// dongle out until a reboot. With nobody else holding a handle, the object goes
// with the process that held it and the next opener creates a fresh one. The
// first run of these cases asserted the abandoned report in the second shape
// and failed, which is how the difference was found.

TEST_CASE("a device lock whose holding process died is taken by the next waiter",
          "[source][lock]") {
    const std::string name = stand_in_name();
    {
        Holder crashed(name, "hold");
        REQUIRE(crashed.read_line() == "held");
        const Onlooker waiting(name);
        crashed.kill();

        auto after = source::DeviceLock::acquire(name, 2s);
        INFO((after ? std::string("held") : after.error().message));
        REQUIRE(after.has_value());
        CHECK(after->recovered_from_abandoned());

        // And it is an ordinary lock from then on: another process sees it
        // held, and free once it is let go.
        CHECK(seen_from_outside(name) == "busy");
        after->release();
        CHECK(seen_from_outside(name) == "free");
    }
}

TEST_CASE("a device lock whose holding process died alone is free to the next opener",
          "[source][lock]") {
    const std::string name = stand_in_name();
    {
        Holder crashed(name, "hold");
        REQUIRE(crashed.read_line() == "held");
        crashed.kill();
    }

    const auto started = std::chrono::steady_clock::now();
    auto after = source::DeviceLock::acquire(name, 2s);
    INFO((after ? std::string("held") : after.error().message));
    REQUIRE(after.has_value());
    CHECK(since(started) < 2s);
    CHECK_FALSE(after->recovered_from_abandoned());
}

TEST_CASE("a second process taking over from a dead holder is told the lock was abandoned",
          "[source][lock]") {
    // The same property from the other side: the recovery is the waiter's to
    // report, whichever process it is.
    const std::string name = stand_in_name();
    Holder crashed(name, "hold");
    REQUIRE(crashed.read_line() == "held");
    const Onlooker waiting(name);
    crashed.kill();

    Holder next(name, "hold");
    CHECK(next.read_line() == "held abandoned");
    next.let_go();
    CHECK(next.read_line() == "released");
}

TEST_CASE("an open under the lock waits its bound, names the lock and never opens the device",
          "[source][lock]") {
    const std::string name = stand_in_name();
    const source::DeviceLockPolicy policy{name, "the stand-in radio", 500ms};

    Holder other(name, "hold");
    REQUIRE(other.read_line() == "held");

    int opens = 0;
    int closes = 0;
    bool held_at_close = false;
    const auto started = std::chrono::steady_clock::now();
    auto opened = source::open_under_lock(policy, [&]() -> Expected<FakeHandle> {
        ++opens;
        return FakeHandle(name, &closes, &held_at_close);
    });
    const auto elapsed = since(started);

    REQUIRE_FALSE(opened.has_value());
    const std::string& message = opened.error().message;
    INFO(message << " after " << elapsed.count() << " ms");
    CHECK(opens == 0);
    CHECK(source::held_elsewhere(opened.error()));
    CHECK(message.find(name) != std::string::npos);
    CHECK(message.find("another process holds the stand-in radio") != std::string::npos);
    CHECK(message.find("500 ms") != std::string::npos);
    CHECK(elapsed >= 450ms);
    CHECK(elapsed < 10s);
}

TEST_CASE("a probe under the lock does not wait and says the device is in use elsewhere",
          "[source][lock]") {
    const std::string name = stand_in_name();
    // An open would wait half a minute here. A probe must not.
    const source::DeviceLockPolicy policy{name, "the stand-in radio", 30s};

    Holder other(name, "hold");
    REQUIRE(other.read_line() == "held");

    int opens = 0;
    int closes = 0;
    bool held_at_close = false;
    const auto started = std::chrono::steady_clock::now();
    auto probed = source::probe_under_lock(policy, [&]() -> Expected<FakeHandle> {
        ++opens;
        return FakeHandle(name, &closes, &held_at_close);
    });
    const auto elapsed = since(started);

    REQUIRE_FALSE(probed.has_value());
    const std::string& message = probed.error().message;
    INFO(message << " after " << elapsed.count() << " ms");
    CHECK(opens == 0);
    CHECK(message.starts_with(source::kProbeHeldElsewhere));
    CHECK(message.find(name) != std::string::npos);
    CHECK(elapsed < 3s);
}

TEST_CASE("the lock is held for a handle's whole life and let go only after it closes",
          "[source][lock]") {
    const std::string name = stand_in_name();
    const source::DeviceLockPolicy policy{name, "the stand-in radio", 2s};

    int opens = 0;
    int closes = 0;
    bool held_at_close = false;
    {
        auto opened = source::open_under_lock(policy, [&]() -> Expected<FakeHandle> {
            ++opens;
            return FakeHandle(name, &closes, &held_at_close);
        });
        INFO((opened ? std::string("opened") : opened.error().message));
        REQUIRE(opened.has_value());
        CHECK(opens == 1);
        CHECK(closes == 0);

        // Not only across the open call: for as long as the handle exists,
        // which for a source is the whole stream.
        CHECK(seen_from_outside(name) == "busy");
        std::this_thread::sleep_for(100ms);
        CHECK(seen_from_outside(name) == "busy");
    }
    CHECK(closes == 1);
    CHECK(held_at_close);
    CHECK_FALSE(source::held_by_this_process(name));
    CHECK(seen_from_outside(name) == "free");
}

TEST_CASE("an open that fails after taking the lock lets it go", "[source][lock]") {
    const std::string name = stand_in_name();
    const source::DeviceLockPolicy policy{name, "the stand-in radio", 2s};

    auto opened = source::open_under_lock(
        policy, []() -> Expected<int> { return fail("the stand-in radio refused to open"); });
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().message == "the stand-in radio refused to open");
    CHECK_FALSE(source::held_elsewhere(opened.error()));
    CHECK_FALSE(source::held_by_this_process(name));
    CHECK(seen_from_outside(name) == "free");
}

TEST_CASE("the radio lock is machine-wide and its open wait is bounded", "[source][lock]") {
    // Global and not Local: the CI runner is a service in session 0 and the
    // owner's engine runs in a desktop session, and Local\ is one namespace per
    // session, so a Local\ lock would serialise neither against the other.
    const source::DeviceLockPolicy policy = source::rtlsdr_lock_policy();
    CHECK(policy.name.starts_with("Global\\"));
    CHECK(policy.name == source::kRtlSdrLockName);
    CHECK(policy.open_wait == source::kRtlSdrOpenWait);
    CHECK(policy.open_wait > 0ms);
    CHECK(policy.open_wait <= 10s);
}

#else  // _WIN32

TEST_CASE("a device lock is shared within a process and let go when its last holder goes",
          "[source][lock]") {
    SKIP("the device lock only arbitrates between processes on Windows so far");
}

#endif  // _WIN32
