#include "tools/webgate/host.h"

#include <cstdio>
#include <fstream>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <io.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

[[nodiscard]] bool usable(HANDLE handle)
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
           GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

}  // namespace

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
