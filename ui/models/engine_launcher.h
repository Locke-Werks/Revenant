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

    // Starts `exe --no-source --port <port>`, plus --token-file when one is
    // named, with no console window and its stderr read by a thread of this
    // object's. Empty on success, the reason otherwise, in words for the
    // window. Called once.
    [[nodiscard]] std::string start(const std::filesystem::path& exe, std::uint16_t port,
                                    const std::filesystem::path& token_file);

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

    std::thread reader_;

    mutable std::mutex mutex_;
    std::vector<std::string> tail_;  // guarded by mutex_
    std::optional<Exit> exit_;       // guarded by mutex_
};

}  // namespace revenant::ui
