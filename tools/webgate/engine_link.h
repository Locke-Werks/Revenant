// Webgate's connection to revenant-engine.
//
// Webgate is an ordinary RPC client of the engine over loopback, the same as
// the Qt client, and this is the half of that which is not specific to a
// phone: find the engine, start one beside it when the rules in
// core/launch/engine_start.h say so, hold one connection, and reconnect when
// it goes.
//
// ONE SHARED CLIENT AND ONE PER PHONE. client() is the link's own connection,
// for what the gateway asks the engine on its own account. It logs in with
// observe, so a gateway with no phone holds no lease and the desktop can start
// while it runs; an observer may call only info, list_sources and
// lease_status. Each phone gets its own Client from new_session(), logging in
// with claim (or takeOver) under that phone's holder text, which is what the
// engine's single-user lease is held by.
//
// THREADS. A supervisor thread owns connecting and the liveness probe.
// on_state callbacks run on it, never under this object's lock, so a callback
// may call back in. client() hands out a raw pointer, which is valid until the
// state leaves Up: a caller that holds it across a state change holds a
// pointer to a Client the supervisor has destroyed, so use it on the thread
// that saw Up, or take a session of its own with new_session().
//
// ON DEMAND (EngineLinkConfig::on_demand), which is how revenant-webgate runs
// from sign-in. The owner, 2026-11-03: "gateway should start at windows
// login. Make sure it also stops the engine completely releasing all of those
// resources when the phone is known gone." So:
//
//   - Nothing is started until a phone claims. The link still observes, so an
//     engine the desktop started is found and used, but a gateway that nobody
//     has asked for holds no GPU context and no radio.
//   - phone_session() is the claim path. It asks for the engine, waits for it,
//     and on an engine this link has just started (with --no-source) opens
//     the desktop's last radio through EngineLinkConfig::reopen, once.
//   - An engine this link started is stopped, by ending its Job object, once
//     leaseStatus has read free for idle_grace and no phone has asked since.
//     A free lease is the phone known gone: a phone's session claims until
//     its linger ends, and keep listening extends the linger while the phone
//     fetches audio, so the lease outlives the socket by exactly the time the
//     phone may come back. It also covers the desktop: a desktop on this
//     engine claims, and a held lease is never stopped under.
//   - An engine this link did not start is never stopped.

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
#include <string_view>
#include <thread>

#include "core/error.h"
#include "core/launch/engine_command.h"
#include "core/launch/engine_launcher.h"
#include "core/rpc/client.h"
#include "core/rpc/token.h"

namespace revenant::webgate {

// The holder text the link's own observing connection logs in with.
inline constexpr std::string_view kLinkHolder = "revenant-webgate";

// How long the lease of an engine the link started must read free before the
// engine is stopped. Long enough that a phone reloading its page, or a desktop
// handing over to a phone, does not pay a cold start; short next to the
// phone's own linger, which has already waited out the phone.
inline constexpr std::chrono::milliseconds kIdleStopGrace{10'000};

// How long a phone's claim waits for an engine this link is starting. The
// engine opens a Vulkan device before it binds, which takes seconds on a cold
// GPU and longer on a loaded one.
inline constexpr std::chrono::milliseconds kDemandWait{20'000};

// The process half of starting an engine, so tests can stand an in-process
// engine in for revenant-engine.exe. The default is launch::EngineLauncher.
class EngineProcess {
public:
    virtual ~EngineProcess() = default;

    // Empty on success, the reason otherwise.
    [[nodiscard]] virtual std::string start(const std::filesystem::path& exe, std::uint16_t port,
                                            const std::filesystem::path& token_file,
                                            const launch::CaptureChoice& capture) = 0;

    // Ends what start() started and returns once it is gone.
    virtual void stop() = 0;

    // Empty while it runs.
    [[nodiscard]] virtual std::optional<launch::EngineLauncher::Exit> exited() const = 0;
};

// Whether the engine should be stopped, from what the link has seen. Pure, so
// the grace is tested without waiting it out.
struct IdleStop {
    std::chrono::milliseconds grace = kIdleStopGrace;
    std::optional<std::chrono::steady_clock::time_point> free_since;

    // One leaseStatus reading. Returns true when the engine should stop now:
    // the lease has read free at every reading since free_since, free_since
    // is grace ago, and nothing has asked for the engine since then either.
    [[nodiscard]] bool observe(bool lease_held, std::chrono::steady_clock::time_point now,
                               std::optional<std::chrono::steady_clock::time_point> last_demand)
    {
        if (lease_held) {
            free_since.reset();
            return false;
        }
        if (!free_since) {
            free_since = now;
        }
        if (last_demand && now - *last_demand < grace) {
            return false;
        }
        return now - *free_since >= grace;
    }

    void reset() { free_since.reset(); }
};

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

    // See ON DEMAND above. Off, an engine is started as soon as nothing
    // answers and is never stopped for want of a phone.
    bool on_demand = false;
    std::chrono::milliseconds idle_grace = kIdleStopGrace;
    std::chrono::milliseconds demand_wait = kDemandWait;

    // Called on a phone's claiming connection when the engine behind it is
    // one this link started and nobody has been handed it since: the place
    // the desktop's last radio is opened. Not called for an engine somebody
    // else started, which already has whatever source they chose.
    std::function<void(rpc::Client&)> reopen;

    // Empty means a launch::EngineLauncher.
    std::function<std::unique_ptr<EngineProcess>()> make_process;
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

    // A connection of its own, so its own engine Session, logged in with mode
    // under holder. Fails with the connect error, category included, when the
    // engine is not there or the token is refused; a claim the lease refuses
    // fails with the engine's "in use: <holder> since <time>". Any thread.
    [[nodiscard]] Expected<std::unique_ptr<rpc::Client>> new_session(
        rpc::LoginMode mode = rpc::LoginMode::Claim, std::string_view holder = {});

    // A phone's claim (or takeOver): asks for the engine when the link is on
    // demand, waits up to demand_wait for it, then new_session(mode, holder),
    // then reopen on an engine this link has just started. Observe goes
    // straight to new_session. Blocks the calling thread while it waits.
    [[nodiscard]] Expected<std::unique_ptr<rpc::Client>> phone_session(rpc::LoginMode mode,
                                                                       std::string_view holder);

    [[nodiscard]] LinkState state() const;

    // phone_session without the wait: asks for the engine and connects to
    // whatever answers now. For the event loop, where a caller that fails
    // tries again on a later tick (the hub's session rebuild).
    [[nodiscard]] Expected<std::unique_ptr<rpc::Client>> phone_session_now(
        rpc::LoginMode mode, std::string_view holder);

    // Releases every phone_session waiting for an engine, now and later, so
    // a shutdown can join the threads waiting in it. Any thread.
    void cancel_waits();

    // Whether an engine this link started is running. Any thread.
    [[nodiscard]] bool owns_engine() const;

    // How many times the link has stopped an engine of its own for want of a
    // phone. For tests and the log.
    [[nodiscard]] std::uint32_t idle_stops() const;

private:
    void supervise(const std::stop_token& stop);
    void publish(LinkState state, std::string detail);
    [[nodiscard]] Expected<rpc::Token> resolve_token() const;
    [[nodiscard]] std::filesystem::path resolved_token_path() const;

    // Starts the engine when the rules allow it. Returns the sentence to show
    // when it did, or when it tried and failed.
    [[nodiscard]] std::optional<std::string> maybe_launch();

    // Drops the connection and ends the engine this link started. Supervisor
    // thread only.
    void stop_own_engine();

    // Whether the supervisor may start an engine now.
    [[nodiscard]] bool may_launch() const;

    EngineLinkConfig config_;

    // Declared before the supervisor so it is destroyed after it.
    std::unique_ptr<EngineProcess> process_;

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable_any changed_;
    std::unique_ptr<rpc::Client> client_;  // guarded by mutex_
    LinkState state_ = LinkState::Starting;  // guarded by mutex_
    std::string detail_;                     // guarded by mutex_
    bool published_ = false;                 // guarded by mutex_
    std::function<void(LinkState, std::string)> callback_;
    bool launched_ = false;                  // guarded by mutex_, written by the supervisor
    std::chrono::steady_clock::time_point launch_not_before_{};  // supervisor only

    // On demand: whether a phone has asked since the last stop, when it last
    // did, whether the supervisor has given up on the current ask, and
    // whether the running engine is one of ours nobody has been handed yet.
    bool demanded_ = false;                  // guarded by mutex_
    // Set under the same lock that reads last_demand_ for the stop decision,
    // and cleared only once the engine is gone, so a claim either lands
    // before the decision (and the stop is not taken) or waits out the stop
    // and starts a new engine. Never a claim on an engine being ended.
    bool stopping_ = false;                  // guarded by mutex_
    bool cancelled_ = false;                 // guarded by mutex_
    bool poked_ = false;                     // guarded by mutex_
    bool demand_failed_ = false;             // guarded by mutex_
    bool fresh_ = false;                     // guarded by mutex_
    std::optional<std::chrono::steady_clock::time_point> last_demand_;  // guarded by mutex_
    std::uint32_t idle_stops_ = 0;           // guarded by mutex_
    IdleStop idle_;                          // supervisor only

    std::jthread supervisor_;
};

}  // namespace revenant::webgate
