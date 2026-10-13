// Starting revenant-engine.exe beside this window, tied to this window's life.
//
// The decision whether to start one is models/engine_start.h and is tested
// there; this is the part that talks to Windows and cannot be, which is why it
// is its own file and why <windows.h> stays in the .cpp. The handles are held
// as void* here so nothing that includes this header inherits the Windows
// headers' macros.
//
// WHAT THE OWNER ASKED FOR, 2026-09-27: an engine the window started stops
// with the window, including when the window crashes. A destructor cannot
// promise the crash half, because a crash runs no destructor. So the engine is
// created inside a Job object whose only handle this object holds, with
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE set. Windows closes every handle a
// process held when it ends, however it ends, and closing the last handle to
// that job terminates everything in it. The destructor closing the handle is
// the ordinary exit taking the same path as the crash.
//
// Construct it before EngineLink and let it be destroyed after: the link's
// destructor talks to the engine on its way out, and the engine has to still
// be there to be talked to.

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "models/capture_choice.h"

namespace revenant::ui {

// Whether anything accepts a TCP connection on 127.0.0.1:port within a short
// wait. The answer to EngineLaunchFacts::port_answers.
//
// A connect and not a look at the listening table, because the question the
// owner's rule asks is whether a client would reach something there, and a
// connect is that question. It opens and closes one connection to whatever is
// listening and sends nothing; an engine's server reads that as a client that
// went away before logging in, which is an event it already handles.
[[nodiscard]] bool loopback_port_answers(std::uint16_t port);

class EngineLauncher {
public:
    EngineLauncher() = default;
    ~EngineLauncher();

    EngineLauncher(const EngineLauncher&) = delete;
    EngineLauncher& operator=(const EngineLauncher&) = delete;
    EngineLauncher(EngineLauncher&&) = delete;
    EngineLauncher& operator=(EngineLauncher&&) = delete;

    // Starts the line models/capture_choice.h's engine_command_line builds,
    // with no console window and its stderr read by a thread of this object's.
    // Empty on success, the reason otherwise, in words for the window.
    //
    // Called again to restart: whatever this object started before is ended
    // first, through stop(), because the new engine binds the same port and
    // the old one has to have let it go.
    [[nodiscard]] std::string start(const std::filesystem::path& exe, std::uint16_t port,
                                    const std::filesystem::path& token_file,
                                    const CaptureChoice& capture);

    // Ends the engine this object started and waits for it to be gone. Does
    // nothing when there is none.
    void stop();

    // The command line of the last start, for the window's stderr.
    [[nodiscard]] const std::string& command_line() const { return command_line_; }

    struct Exit {
        std::uint32_t code = 0;

        // The last few lines the engine wrote to stderr, oldest first. See
        // engine_exit_sentence for which of them is the reason.
        std::vector<std::string> tail;
    };

    // Empty while the engine runs. Any thread.
    [[nodiscard]] std::optional<Exit> exited() const;

    [[nodiscard]] std::uint32_t pid() const { return pid_; }

private:
    // The reader thread: stderr until the pipe closes, then the exit code.
    void drain();

    void* job_ = nullptr;
    void* process_ = nullptr;
    void* stderr_read_ = nullptr;
    std::uint32_t pid_ = 0;
    std::string command_line_;

    std::thread reader_;

    mutable std::mutex mutex_;
    std::vector<std::string> tail_;  // guarded by mutex_
    std::optional<Exit> exit_;       // guarded by mutex_
};

}  // namespace revenant::ui
