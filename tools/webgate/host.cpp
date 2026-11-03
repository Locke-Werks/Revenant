#include "tools/webgate/host.h"

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <io.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>

namespace revenant::webgate {

RotatingLog::RotatingLog(std::filesystem::path path, std::uintmax_t max_bytes)
    : path_(std::move(path)), max_bytes_(max_bytes)
{
}

std::filesystem::path RotatingLog::previous_path() const
{
    std::filesystem::path older = path_;
    older += ".1";
    return older;
}

void RotatingLog::append(std::string_view text)
{
    std::error_code ignored;
    if (!size_) {
        std::filesystem::create_directories(path_.parent_path(), ignored);
        const auto existing = std::filesystem::file_size(path_, ignored);
        size_ = ignored ? 0 : existing;
    }
    if (*size_ > 0 && *size_ + text.size() > max_bytes_) {
        // rename over an existing file is a replace on Windows through
        // std::filesystem, so the older file is simply the one before this.
        std::filesystem::rename(path_, previous_path(), ignored);
        // Only on a rename that happened: a reader holding the file without
        // delete sharing makes it fail, and the size is then still the file's.
        if (!ignored) {
            size_ = 0;
        }
    }
    // Opened per write so the file can be read, renamed or deleted while the
    // gateway runs. Writes are a line or a few at a time, so the open is cheap
    // next to what it is logging.
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        return;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    *size_ += text.size();
}

std::unique_ptr<SingleInstance> SingleInstance::acquire(const std::string& name)
{
    const std::wstring wide(name.begin(), name.end());
    HANDLE handle = CreateMutexW(nullptr, TRUE, wide.c_str());
    if (handle == nullptr) {
        return nullptr;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        return nullptr;
    }
    return std::unique_ptr<SingleInstance>(new SingleInstance(handle));
}

SingleInstance::~SingleInstance()
{
    if (handle_ != nullptr) {
        ReleaseMutex(static_cast<HANDLE>(handle_));
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

std::string instance_name(std::uint16_t port)
{
    return "Local\\Revenant-webgate-" + std::to_string(port);
}

namespace {

// SYNCHRONIZE | EVENT_MODIFY_STATE: wait and set, nothing else.
constexpr DWORD kStopAccess = SYNCHRONIZE | EVENT_MODIFY_STATE;

[[nodiscard]] std::wstring widen(std::string_view text)
{
    return std::wstring(text.begin(), text.end());
}

// The account a token runs as, as a string SID, or empty.
[[nodiscard]] std::wstring token_user_sid(HANDLE token)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> buffer(size);
    if (size == 0 ||
        GetTokenInformation(token, TokenUser, buffer.data(), size, &size) == 0) {
        return {};
    }
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text) ==
        0) {
        return {};
    }
    std::wstring out(text);
    LocalFree(text);
    return out;
}

// The DACL this process's stop event is created with. See kStopEventPrefix.
[[nodiscard]] std::wstring own_stop_sddl()
{
    HANDLE token = nullptr;
    std::wstring user;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0) {
        user = token_user_sid(token);
        CloseHandle(token);
    }
    std::wstring sddl = L"D:P(A;;0x100002;;;SY)(A;;0x100002;;;BA)";
    if (!user.empty()) {
        sddl += L"(A;;0x100002;;;" + user + L")";
    }
    return sddl;
}

// Best effort: lets an elevated --stop open gateways of other accounts,
// whose process DACLs name only their own user and SYSTEM.
void set_debug_privilege(bool on)
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) ==
        0) {
        return;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid) != 0) {
        privileges.Privileges[0].Attributes = on ? SE_PRIVILEGE_ENABLED : 0;
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    }
    CloseHandle(token);
}

// The account process runs as, as a binary SID, or empty.
[[nodiscard]] std::vector<std::uint8_t> process_user(HANDLE process)
{
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token) == 0) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> buffer(size);
    std::vector<std::uint8_t> sid;
    if (size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0) {
        PSID user = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
        const auto* bytes = static_cast<const std::uint8_t*>(user);
        sid.assign(bytes, bytes + GetLengthSid(user));
    }
    CloseHandle(token);
    return sid;
}

[[nodiscard]] bool usable(HANDLE handle)
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
           GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

}  // namespace

std::string stop_event_name(std::string_view prefix, std::uint32_t pid)
{
    return std::string(prefix) + std::to_string(pid);
}

bool stop_event_owner_acceptable(void* owner, void* process_user_sid)
{
    if (owner == nullptr || IsValidSid(owner) == 0) {
        return false;
    }
    if (process_user_sid != nullptr && IsValidSid(process_user_sid) != 0 &&
        EqualSid(owner, process_user_sid) != 0) {
        return true;
    }
    // An elevated gateway's objects are owned by Administrators, not by its
    // user; SYSTEM owns what a service creates.
    return IsWellKnownSid(owner, WinBuiltinAdministratorsSid) != 0 ||
           IsWellKnownSid(owner, WinLocalSystemSid) != 0;
}

StopWatch::StopWatch(std::function<void()> on_stop, std::string prefix)
{
    const std::wstring name =
        L"Local\\" + widen(stop_event_name(prefix, GetCurrentProcessId()));
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            own_stop_sddl().c_str(), SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        return;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    HANDLE event = CreateEventW(&attributes, TRUE, FALSE, name.c_str());
    const DWORD created = GetLastError();
    LocalFree(descriptor);
    if (event == nullptr) {
        return;
    }
    if (created == ERROR_ALREADY_EXISTS) {
        // A pid names one live process, so an event under this one's name
        // that already exists was made by somebody else, ahead of it.
        std::fprintf(stderr, "revenant-webgate: a stop event under this gateway's name already "
                             "exists and is not its own; not watching it\n");
        CloseHandle(event);
        return;
    }
    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (quit == nullptr) {
        CloseHandle(event);
        return;
    }
    event_ = event;
    quit_ = quit;
    thread_ = std::thread([event, quit, on_stop = std::move(on_stop)] {
        const HANDLE both[2] = {quit, event};
        if (WaitForMultipleObjects(2, both, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            on_stop();
        }
    });
}

StopWatch::~StopWatch()
{
    if (quit_ != nullptr) {
        SetEvent(static_cast<HANDLE>(quit_));
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(event_));
    }
    if (quit_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(quit_));
    }
}

namespace {

[[nodiscard]] StopSignal signal_with(std::uint32_t pid, HANDLE process, std::string_view prefix);

[[nodiscard]] bool elevated()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool yes = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation,
                                         &size) != 0 &&
                     elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return yes;
}

[[nodiscard]] std::filesystem::path image_path(HANDLE process)
{
    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size) == 0) {
        return {};
    }
    buffer.resize(size);
    return std::filesystem::path(buffer);
}

[[nodiscard]] bool same_directory(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ignored;
    const auto left = std::filesystem::weakly_canonical(a.parent_path(), ignored).wstring();
    const auto right = std::filesystem::weakly_canonical(b.parent_path(), ignored).wstring();
    return !left.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
}

}  // namespace

StopSignal signal_gateway(std::uint32_t pid, std::string_view prefix)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    const StopSignal result = signal_with(pid, process, prefix);
    if (process != nullptr) {
        CloseHandle(process);
    }
    return result;
}

namespace {

StopSignal signal_with(std::uint32_t pid, HANDLE process, std::string_view prefix)
{
    DWORD session = 0;
    if (ProcessIdToSessionId(pid, &session) == 0) {
        return StopSignal::NoEvent;
    }
    // Session\<n>\ reaches that session's Local\ namespace from any session.
    const std::wstring name =
        L"Session\\" + std::to_wstring(session) + L"\\" + widen(stop_event_name(prefix, pid));
    HANDLE event = OpenEventW(kStopAccess | READ_CONTROL, FALSE, name.c_str());
    if (event == nullptr) {
        return StopSignal::NoEvent;
    }

    std::vector<std::uint8_t> user;
    if (process != nullptr) {
        user = process_user(process);
    }
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD read = GetSecurityInfo(event, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
                                       &owner, nullptr, nullptr, nullptr, &descriptor);
    const bool trusted =
        read == ERROR_SUCCESS &&
        stop_event_owner_acceptable(owner, user.empty() ? nullptr : user.data());
    if (descriptor != nullptr) {
        LocalFree(descriptor);
    }
    if (!trusted) {
        CloseHandle(event);
        return StopSignal::Impostor;
    }
    SetEvent(event);
    CloseHandle(event);
    return StopSignal::Set;
}

}  // namespace

int stop_running_gateways(std::chrono::milliseconds wait, std::string prefix, std::wstring image)
{
    // Held only while the handles are opened, then given up: everything
    // after that works through handles already granted.
    set_debug_privilege(true);

    // An elevated --stop terminates only gateways installed where it is, so
    // the installer's hook cannot end some other copy that merely shares the
    // image name. Unelevated it reaches only the caller's own processes.
    const bool admin = elevated();
    std::filesystem::path self;
    if (HANDLE me = GetCurrentProcess()) {
        self = image_path(me);
    }

    // Every gateway on the machine but this one, found by image name, which
    // is what the installer's files are held open by.
    struct Target {
        HANDLE process;
        bool may_terminate;
    };
    std::vector<Target> targets;
    int unreachable = 0;
    int impostors = 0;
    int elsewhere = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof entry;
        for (BOOL more = Process32FirstW(snapshot, &entry); more != 0;
             more = Process32NextW(snapshot, &entry)) {
            if (entry.th32ProcessID == GetCurrentProcessId() ||
                _wcsicmp(entry.szExeFile, image.c_str()) != 0) {
                continue;
            }
            HANDLE process =
                OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                            FALSE, entry.th32ProcessID);
            if (process == nullptr) {
                ++unreachable;
                continue;
            }
            if (signal_with(entry.th32ProcessID, process, prefix) == StopSignal::Impostor) {
                ++impostors;
            }
            bool may_terminate = true;
            if (admin) {
                const auto where = image_path(process);
                may_terminate = same_directory(where, self);
                if (!may_terminate) {
                    std::printf("revenant-webgate --stop: not terminating pid %lu at %ls, which "
                                "is not this installation's\n",
                                entry.th32ProcessID, where.c_str());
                }
            }
            targets.push_back(Target{process, may_terminate});
        }
        CloseHandle(snapshot);
    }
    set_debug_privilege(false);

    const auto until = std::chrono::steady_clock::now() + wait;
    int exited = 0;
    int terminated = 0;
    int remaining = 0;
    for (const Target& target : targets) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            until - std::chrono::steady_clock::now());
        const DWORD ms = left.count() > 0 ? static_cast<DWORD>(left.count()) : 0;
        if (WaitForSingleObject(target.process, ms) == WAIT_OBJECT_0) {
            ++exited;
        } else if (!target.may_terminate) {
            // Signalled and left alone: another installation's files, not
            // the ones this install replaces.
            ++elsewhere;
        } else {
            // No event (a version from before --stop), an impostor's, or a
            // gateway that did not finish. Its engine's Job closes with it.
            TerminateProcess(target.process, 1);
            if (WaitForSingleObject(target.process, 5'000) == WAIT_OBJECT_0) {
                ++terminated;
            } else {
                ++remaining;
            }
        }
        CloseHandle(target.process);
    }

    std::printf("revenant-webgate --stop: %d exited, %d terminated, %d still running, %d not "
                "reachable, %d impostor events ignored, %d of other installations left running\n",
                exited, terminated, remaining, unreachable, impostors, elsewhere);
    return remaining == 0 && unreachable == 0 ? 0 : 1;
}

OutputTee::OutputTee(std::filesystem::path log_path) : log_(std::move(log_path))
{
    // Handles a parent passed (a test's files, a pipe, a redirect) win over
    // a console, because that is where the parent is reading. A Windows-
    // subsystem program started from a terminal is given neither, so the
    // terminal's console is attached to by hand.
    if (!usable(GetStdHandle(STD_OUTPUT_HANDLE))) {
        if (AttachConsole(ATTACH_PARENT_PROCESS) != 0) {
            FILE* ignored = nullptr;
            static_cast<void>(freopen_s(&ignored, "CONOUT$", "w", stdout));
            static_cast<void>(freopen_s(&ignored, "CONOUT$", "w", stderr));
        }
    }

    const int out_fd = _fileno(stdout);
    if (out_fd >= 0 && usable(reinterpret_cast<HANDLE>(_get_osfhandle(out_fd)))) {
        forward_fd_ = _dup(out_fd);
    }

    // stdout and stderr need descriptors to be pointed at the pipe; a
    // program with none has a stream with no descriptor behind it.
    FILE* ignored = nullptr;
    if (_fileno(stdout) < 0) {
        static_cast<void>(freopen_s(&ignored, "NUL", "w", stdout));
    }
    if (_fileno(stderr) < 0) {
        static_cast<void>(freopen_s(&ignored, "NUL", "w", stderr));
    }

    int fds[2] = {-1, -1};
    // Not inheritable: a child holding the write end would keep the reader
    // from ever seeing end of file, and the destructor's join would hang.
    if (_pipe(fds, 64 * 1024, _O_BINARY | _O_NOINHERIT) != 0) {
        return;
    }
    read_fd_ = fds[0];
    std::fflush(stdout);
    std::fflush(stderr);
    _dup2(fds[1], _fileno(stdout));
    _dup2(fds[1], _fileno(stderr));
    // _dup2 makes inheritable copies whatever the original was.
    for (const int fd : {_fileno(stdout), _fileno(stderr)}) {
        SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), HANDLE_FLAG_INHERIT, 0);
    }
    _close(fds[1]);
    // Unbuffered, so a line reaches the log when it is written and not when
    // a buffer fills, and stdout and stderr interleave in the order written.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    reader_ = std::thread([this] { drain(); });
}

OutputTee::~OutputTee()
{
    if (reader_.joinable()) {
        std::fflush(stdout);
        std::fflush(stderr);
        // The last write ends of the pipe, so the reader sees end of file.
        _close(_fileno(stdout));
        _close(_fileno(stderr));
        reader_.join();
    }
    if (read_fd_ >= 0) {
        _close(read_fd_);
    }
    if (forward_fd_ >= 0) {
        _close(forward_fd_);
    }
}

void OutputTee::drain()
{
    char buffer[4096];
    for (;;) {
        const int got = _read(read_fd_, buffer, sizeof buffer);
        if (got <= 0) {
            return;
        }
        if (forward_fd_ >= 0) {
            static_cast<void>(_write(forward_fd_, buffer, static_cast<unsigned>(got)));
        }
        log_.append(std::string_view(buffer, static_cast<std::size_t>(got)));
    }
}

}  // namespace revenant::webgate
