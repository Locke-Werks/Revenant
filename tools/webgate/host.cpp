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
        size_ = 0;
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

// See kStopEventName for why each part is there. 0x100000 is SYNCHRONIZE.
constexpr const wchar_t* kStopEventSddl =
    L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)(A;;0x100000;;;WD)S:(ML;;NW;;;LW)";

[[nodiscard]] HANDLE open_stop_event(const std::string& name)
{
    const std::wstring wide(name.begin(), name.end());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kStopEventSddl, SDDL_REVISION_1,
                                                             &descriptor, nullptr) == 0) {
        descriptor = nullptr;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    HANDLE event = CreateEventW(descriptor != nullptr ? &attributes : nullptr, TRUE, FALSE,
                                wide.c_str());
    if (event == nullptr) {
        // Exists, made by an account whose DACL gives this one SYNCHRONIZE
        // and nothing else.
        event = OpenEventW(SYNCHRONIZE, FALSE, wide.c_str());
    }
    if (descriptor != nullptr) {
        LocalFree(descriptor);
    }
    return event;
}

[[nodiscard]] bool usable(HANDLE handle)
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
           GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

}  // namespace

StopWatch::StopWatch(std::function<void()> on_stop, std::string name)
{
    HANDLE event = open_stop_event(name);
    if (event == nullptr) {
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

int stop_running_gateways(std::chrono::milliseconds wait, std::string name, std::wstring image)
{
    HANDLE event = open_stop_event(name);
    if (event == nullptr || SetEvent(event) == 0) {
        std::printf("revenant-webgate --stop: could not signal running gateways (error %lu)\n",
                    GetLastError());
        if (event != nullptr) {
            CloseHandle(event);
        }
        return 2;
    }

    // Every gateway on the machine but this one, found by image name, which
    // is what the installer's files are held open by.
    std::vector<HANDLE> running;
    int unreachable = 0;
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
            if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID)) {
                running.push_back(process);
            } else {
                ++unreachable;
            }
        }
        CloseHandle(snapshot);
    }

    const auto until = std::chrono::steady_clock::now() + wait;
    int remaining = 0;
    for (HANDLE process : running) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            until - std::chrono::steady_clock::now());
        const DWORD ms = left.count() > 0 ? static_cast<DWORD>(left.count()) : 0;
        if (WaitForSingleObject(process, ms) != WAIT_OBJECT_0) {
            ++remaining;
        }
        CloseHandle(process);
    }

    // Cleared, or a gateway started after this (the next sign-in) would see
    // the old signal and exit at once. The event lives only while a handle
    // to it does, so this matters only while some gateway still holds one.
    ResetEvent(event);
    CloseHandle(event);

    std::printf("revenant-webgate --stop: %zu stopped, %d still running, %d not reachable\n",
                running.size() - static_cast<std::size_t>(remaining), remaining, unreachable);
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
    if (_pipe(fds, 64 * 1024, _O_BINARY) != 0) {
        return;
    }
    read_fd_ = fds[0];
    std::fflush(stdout);
    std::fflush(stderr);
    _dup2(fds[1], _fileno(stdout));
    _dup2(fds[1], _fileno(stderr));
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
