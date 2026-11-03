// Webgate's connection to revenant-engine.
//
// Webgate is an ordinary RPC client of the engine over loopback, the same as
// the Qt client, and this is the half of that which is not specific to a
// phone: find the engine, start one beside it when the rules in
// core/launch/engine_start.h say so, hold one connection, and reconnect when
// it goes.
//
// ONE SHARED CLIENT AND ONE PER PHONE. client() is the link's own connection,
// for what the gateway asks the engine on its own account. Each phone gets its
// own Client from new_session(), because a Client is a Session on the engine
// and a Session owns its receivers: two phones sharing one would share their
// receivers and each one's disconnect would remove the other's.
//
// THREADS. A supervisor thread owns connecting and the liveness probe.
// on_state callbacks run on it, never under this object's lock, so a callback
// may call back in. client() hands out a raw pointer, which is valid until the
// state leaves Up: a caller that holds it across a state change holds a
// pointer to a Client the supervisor has destroyed, so use it on the thread
// that saw Up, or take a session of its own with new_session().

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/error.h"
#include "core/launch/engine_command.h"
#include "core/launch/engine_launcher.h"
#include "core/rpc/client.h"
#include "core/rpc/token.h"

namespace revenant::webgate {

enum class LinkState : std::uint8_t {
    // An engine this link started is coming up, or the first attempt is
    // still in flight.
    Starting,

    // Nothing to connect to: no engine on the port and none this link may
    // start, or one that has not bound yet.
    Waiting,

    Up,

    // The engine answered and will not take the token (ErrorCategory::
    // Unauthenticated). Retried at refused_interval, not at poll_interval.
    Refused,
};

struct EngineLinkConfig {
    std::string address = "127.0.0.1";
    std::uint16_t port = 17690;

    // A fixed token, for tests, which must not touch a real profile. When
    // empty the token is read from token_path.
    std::optional<rpc::Token> token;

    // Empty means rpc::default_token_path(). Read afresh on every attempt, so
    // a token minted or rotated after webgate started is picked up by the
    // next attempt and not by a restart. Also passed to an engine this link
    // starts.
    std::filesystem::path token_path;

    // The engine to start when nothing answers on the port. Empty means never
    // start one, which is also the answer when address is not one an engine
    // started here would be reachable at.
    std::filesystem::path engine_exe;
    launch::CaptureChoice capture;

    // The liveness probe, and the retry while nothing answers.
    std::chrono::milliseconds poll_interval{1000};

    // How long a refused token waits before being offered again. A backoff
    // and not a stop: the token is resolved per attempt, so fixing the file
    // takes effect without a restart. Nothing about asking again changes the
    // answer while the file is unchanged, so it is not asked every second.
    std::chrono::milliseconds refused_interval{10'000};

    // How long after an engine this link started has exited before it is
    // started again. An engine that dies on launch would otherwise be
    // relaunched every poll_interval.
    std::chrono::milliseconds restart_interval{5'000};
};

class EngineLink {
public:
    explicit EngineLink(EngineLinkConfig config);

    // Stops the supervisor, drops the connection, then ends any engine this
    // link started, in that order: the Client's teardown talks to the engine.
    ~EngineLink();

    EngineLink(const EngineLink&) = delete;
    EngineLink& operator=(const EngineLink&) = delete;
    EngineLink(EngineLink&&) = delete;
    EngineLink& operator=(EngineLink&&) = delete;

    // Called on every change of state or detail, from the supervisor thread.
    // Set before start(); not safe to replace while running.
    void on_state(std::function<void(LinkState, std::string detail)> callback);

    // Begins connecting. Returns at once. A second call does nothing.
    void start();

    // The link's own connection, null while not Up. See THREADS above.
    [[nodiscard]] rpc::Client* client();

    // A connection of its own, so its own engine Session. Fails with the
    // connect error, category included, when the engine is not there or the
    // token is refused. Any thread.
    [[nodiscard]] Expected<std::unique_ptr<rpc::Client>> new_session();

    [[nodiscard]] LinkState state() const;

private:
    void supervise(const std::stop_token& stop);
    void publish(LinkState state, std::string detail);
    [[nodiscard]] Expected<rpc::Token> resolve_token() const;
    [[nodiscard]] std::filesystem::path resolved_token_path() const;

    // Starts the engine when the rules allow it. Returns the sentence to show
    // when it did, or when it tried and failed.
    [[nodiscard]] std::optional<std::string> maybe_launch();

    EngineLinkConfig config_;

    // Declared before the supervisor so it is destroyed after it.
    launch::EngineLauncher launcher_;

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::unique_ptr<rpc::Client> client_;  // guarded by mutex_
    LinkState state_ = LinkState::Starting;  // guarded by mutex_
    std::string detail_;                     // guarded by mutex_
    bool published_ = false;                 // guarded by mutex_
    std::function<void(LinkState, std::string)> callback_;
    bool launched_ = false;                  // supervisor thread only
    std::chrono::steady_clock::time_point launch_not_before_{};  // supervisor only

    std::jthread supervisor_;
};

}  // namespace revenant::webgate
