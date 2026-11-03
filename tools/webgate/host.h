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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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

// THE STOP SIGNAL, for the installer. An upgrade must not swap the files under
// a running gateway, and the gateway runs at sign-in with no window to close,
// so the installer's pre_install hook runs `revenant-webgate.exe --stop`.
//
// ONE EVENT PER GATEWAY, NOT ONE FOR THE MACHINE. Each gateway creates
// Local\Revenant-webgate-stop-<pid> in its own session's namespace, with a
// DACL granting set and wait to its own account, Administrators and SYSTEM
// only. So no other user can stop it, and nobody can squat a machine-wide
// name to stop every gateway at start or to make --stop fail. A name that
// already exists when the gateway creates it is somebody else's, and the
// gateway does not watch it.
//
// --stop finds each revenant-webgate.exe, opens its event through that
// process's session (Session\<n>\...), checks the event's owner is that
// process's account (or Administrators or SYSTEM) so an impostor is ignored,
// sets it, and waits. A gateway still running at the end of the wait is
// terminated: its engine's Job object closes with it (KILL_ON_JOB_CLOSE), so
// the engine goes too. Elevated, from the installer, that reaches every
// session; unelevated it reaches only the caller's own gateways.
inline constexpr std::string_view kStopEventPrefix = "Revenant-webgate-stop-";

// The name a gateway with this pid creates, in the Local\ namespace.
[[nodiscard]] std::string stop_event_name(std::string_view prefix, std::uint32_t pid);

// Whether an event whose owner is owner may be trusted as the stop event of a
// process running as process_user. Both are SIDs.
[[nodiscard]] bool stop_event_owner_acceptable(void* owner, void* process_user);

// Watches this process's stop event on a thread of its own and calls on_stop
// once when it is set. Destroying it ends the watch without calling.
class StopWatch {
public:
    StopWatch(std::function<void()> on_stop,
              std::string prefix = std::string(kStopEventPrefix));
    ~StopWatch();
    StopWatch(const StopWatch&) = delete;
    StopWatch& operator=(const StopWatch&) = delete;

    // Whether the event could be opened at all. Without it the gateway runs,
    // and only Task Manager stops it.
    [[nodiscard]] bool watching() const { return event_ != nullptr; }

private:
    void* event_ = nullptr;
    void* quit_ = nullptr;
    std::thread thread_;
};

enum class StopSignal : std::uint8_t { Set, NoEvent, Impostor };

// Sets the stop event of the gateway with this pid, after the owner check.
[[nodiscard]] StopSignal signal_gateway(std::uint32_t pid,
                                        std::string_view prefix = kStopEventPrefix);

// --stop: signals every other process named image, waits up to wait for them
// to exit, then terminates those still running. 0 when none remain; 1 when
// one could not be reached (another account's, when not elevated) or would
// not die. Messages go to stdout.
[[nodiscard]] int stop_running_gateways(std::chrono::milliseconds wait,
                                        std::string prefix = std::string(kStopEventPrefix),
                                        std::wstring image = L"revenant-webgate.exe");

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
