// What revenant-webgate needs to run as a sign-in program rather than from a
// terminal: one copy per sign-in session, and a log file to read afterwards.
//
// The executable is a Windows-subsystem program so that the HKLM Run entry
// the installer writes opens no console at sign-in. A program like that has
// no stdout of its own, so everything written to stdout and stderr, by this
// file's caller and by every unit that prints, goes through a pipe to
// OutputTee's thread, which appends it to the log and passes it on to
// whatever stdout the process was given: a parent's console, or the files a
// test harness handed over.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace revenant::webgate {

// The log beside the certificates: %LOCALAPPDATA%\Revenant\webgate\webgate.log.
inline constexpr std::string_view kLogName = "webgate.log";

// Keeps the last max_bytes and one older file of the same size, as name.1.
// Rotation is by rename before the write that would cross the cap, so a line
// is never split between two files.
class RotatingLog {
public:
    RotatingLog(std::filesystem::path path, std::uintmax_t max_bytes = 1024 * 1024);

    void append(std::string_view text);

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::filesystem::path previous_path() const;

private:
    std::filesystem::path path_;
    std::uintmax_t max_bytes_;
    std::optional<std::uintmax_t> size_;
};

// A named mutex held for the life of this object. The name is in the Local\
// namespace, which is per sign-in session, so two people signed in to one
// machine each get a gateway; two gateways in one session on the same port
// would only fight over the bind.
class SingleInstance {
public:
    // Acquired when no other holder of name exists in this session.
    [[nodiscard]] static std::unique_ptr<SingleInstance> acquire(const std::string& name);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

private:
    explicit SingleInstance(void* handle) : handle_(handle) {}
    void* handle_;
};

// The mutex name for a gateway serving HTTPS on port.
[[nodiscard]] std::string instance_name(std::uint16_t port);

// Takes over stdout and stderr; see the top of this file. Construct first
// thing in main, before anything prints.
class OutputTee {
public:
    explicit OutputTee(std::filesystem::path log_path);

    // Restores nothing: it closes the pipe so the thread drains what is left
    // into the log, and joins it.
    ~OutputTee();
    OutputTee(const OutputTee&) = delete;
    OutputTee& operator=(const OutputTee&) = delete;

    // Whether there was somewhere to pass output on to.
    [[nodiscard]] bool has_console() const { return forward_fd_ >= 0; }

private:
    void drain();

    RotatingLog log_;
    int read_fd_ = -1;
    int forward_fd_ = -1;
    std::thread reader_;
};

}  // namespace revenant::webgate
