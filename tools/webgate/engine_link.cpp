#include "tools/webgate/engine_link.h"

#include <cstdio>
#include <span>
#include <system_error>
#include <utility>

#include "core/launch/engine_start.h"

namespace revenant::webgate {

namespace {

class LauncherProcess final : public EngineProcess {
public:
    std::string start(const std::filesystem::path& exe, std::uint16_t port,
                      const std::filesystem::path& token_file,
                      const launch::CaptureChoice& capture) override
    {
        return launcher_.start(exe, port, token_file, capture);
    }

    void stop() override { launcher_.stop(); }

    [[nodiscard]] std::optional<launch::EngineLauncher::Exit> exited() const override
    {
        return launcher_.exited();
    }

private:
    launch::EngineLauncher launcher_;
};

}  // namespace

EngineLink::EngineLink(EngineLinkConfig config)
    : config_(std::move(config)),
      process_(config_.make_process ? config_.make_process()
                                    : std::make_unique<LauncherProcess>())
{
}

EngineLink::~EngineLink()
{
    // Joined before anything else goes, so the supervisor is not mid-connect
    // while the Client and the launcher it uses are torn down.
    supervisor_.request_stop();
    if (supervisor_.joinable()) {
        supervisor_.join();
    }

    std::unique_ptr<rpc::Client> client;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        client = std::move(client_);
    }
    // The Client goes before the process (a member, destroyed after this
    // body): its teardown talks to the engine, which has to still be there.
    client.reset();
}

void EngineLink::on_state(std::function<void(LinkState, std::string)> callback)
{
    callback_ = std::move(callback);
}

void EngineLink::start()
{
    if (supervisor_.joinable()) {
        return;
    }
    supervisor_ = std::jthread([this](const std::stop_token& stop) { supervise(stop); });
}

rpc::Client* EngineLink::client()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return client_.get();
}

LinkState EngineLink::state() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

Expected<std::unique_ptr<rpc::Client>> EngineLink::phone_session_now(rpc::LoginMode mode,
                                                                     std::string_view holder)
{
    if (mode != rpc::LoginMode::Observe && config_.on_demand) {
        const std::lock_guard<std::mutex> lock(mutex_);
        last_demand_ = std::chrono::steady_clock::now();
        demanded_ = true;
        poked_ = true;
        if (stopping_) {
            // The engine is being ended; connecting now would reach it on
            // its way out. The caller tries again, and by then there is a
            // new one.
            Error busy;
            busy.message = "the engine is restarting for this phone";
            busy.category = ErrorCategory::Unreachable;
            wake_.notify_all();
            return std::unexpected(busy);
        }
    }
    wake_.notify_all();
    auto client = new_session(mode, holder);
    if (client) {
        bool fresh = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            fresh = std::exchange(fresh_, false);
        }
        if (fresh && config_.reopen) {
            config_.reopen(**client);
        }
    }
    return client;
}

void EngineLink::cancel_waits()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
    }
    changed_.notify_all();
}

bool EngineLink::owns_engine() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return launched_ && !process_->exited();
}

std::uint32_t EngineLink::idle_stops() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return idle_stops_;
}

std::filesystem::path EngineLink::resolved_token_path() const
{
    if (!config_.token_path.empty()) {
        return config_.token_path;
    }
    if (auto path = rpc::default_token_path(); path) {
        return *path;
    }
    return {};
}

Expected<rpc::Token> EngineLink::resolve_token() const
{
    if (config_.token) {
        return *config_.token;
    }
    if (!config_.token_path.empty()) {
        return rpc::load_token(config_.token_path.string());
    }
    auto path = rpc::default_token_path();
    if (!path) {
        return std::unexpected(path.error());
    }
    return rpc::load_token(*path);
}

Expected<std::unique_ptr<rpc::Client>> EngineLink::new_session(rpc::LoginMode mode,
                                                               std::string_view holder)
{
    auto token = resolve_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    return rpc::Client::connect(config_.address, config_.port,
                                std::span<const std::uint8_t>(token->data(), token->size()),
                                mode, holder);
}

Expected<std::unique_ptr<rpc::Client>> EngineLink::phone_session(rpc::LoginMode mode,
                                                                 std::string_view holder)
{
    if (mode == rpc::LoginMode::Observe) {
        return new_session(mode, holder);
    }

    if (config_.on_demand) {
        std::unique_lock<std::mutex> lock(mutex_);
        last_demand_ = std::chrono::steady_clock::now();
        demanded_ = true;
        demand_failed_ = false;
        poked_ = true;
        wake_.notify_all();
        // Up is the engine answering the link; Refused and a failed launch
        // will not change by waiting, and the connect below reports them.
        // While a stop is under way Up is the engine being ended, so this
        // waits it out; the stop leaves demanded_ alone, and the supervisor
        // then starts a new engine for this ask.
        changed_.wait_for(lock, config_.demand_wait, [this] {
            return cancelled_ ||
                   (!stopping_ && (state_ == LinkState::Up || state_ == LinkState::Refused ||
                                   demand_failed_));
        });
    }

    auto client = new_session(mode, holder);
    if (!client) {
        return client;
    }

    bool fresh = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // A demand is also a phone being here, so the grace starts again from
        // now and not from whenever the lease last read free.
        last_demand_ = std::chrono::steady_clock::now();
        fresh = std::exchange(fresh_, false);
    }
    if (fresh && config_.reopen) {
        config_.reopen(**client);
    }
    return client;
}

void EngineLink::publish(LinkState state, std::string detail)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (published_ && state_ == state && detail_ == detail) {
            return;
        }
        published_ = true;
        state_ = state;
        detail_ = detail;
    }
    changed_.notify_all();
    // Outside the lock, so a callback that asks the link a question does not
    // deadlock on it.
    if (callback_) {
        callback_(state, std::move(detail));
    }
}

bool EngineLink::may_launch() const
{
    if (!config_.on_demand) {
        return true;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    return demanded_;
}

std::optional<std::string> EngineLink::maybe_launch()
{
    if (launched_) {
        if (!process_->exited()) {
            return std::nullopt;  // Ours, still coming up.
        }
        if (std::chrono::steady_clock::now() < launch_not_before_) {
            return launch::engine_exit_sentence(process_->exited()->code,
                                                process_->exited()->tail);
        }
    }
    if (!may_launch()) {
        return std::nullopt;
    }

    launch::EngineLaunchFacts facts;
    facts.launchable_address = launch::is_launchable_address(config_.address);
    std::error_code ignored;
    facts.engine_beside = !config_.engine_exe.empty() &&
                          std::filesystem::exists(config_.engine_exe, ignored);
    // Probed only when the earlier rules pass, because it costs a connect.
    facts.port_answers = facts.launchable_address && facts.engine_beside &&
                         launch::loopback_port_answers(config_.port);
    if (launch::plan_engine_launch(facts) != launch::EngineLaunch::Start) {
        return std::nullopt;
    }

    launch_not_before_ = std::chrono::steady_clock::now() + config_.restart_interval;
    std::string refusal = process_->start(config_.engine_exe, config_.port,
                                          resolved_token_path(), config_.capture);
    if (!refusal.empty()) {
        return refusal;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    launched_ = true;
    // Started with --no-source, so the first phone opens the desktop's radio.
    fresh_ = true;
    return std::nullopt;
}

void EngineLink::stop_own_engine()
{
    std::unique_ptr<rpc::Client> gone;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        gone = std::move(client_);
    }
    // The link's observing connection first, while the engine can still
    // answer its goodbye.
    gone.reset();

    // EngineLauncher::stop terminates the Job object, so the engine and
    // anything it started go together, then waits on the process handle. A
    // process that has exited has released its device handles, which is the
    // dongle and the GPU context both: Windows closes every handle at exit.
    process_->stop();

    std::string detail = "the engine is stopped until a phone connects";
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        launched_ = false;
        fresh_ = false;
        ++idle_stops_;
        idle_.reset();
        // Waiting before stopping_ clears, in the same critical section, so a
        // claim that waited out the stop does not see the old Up and connect
        // to nothing.
        published_ = true;
        state_ = LinkState::Waiting;
        detail_ = detail;
        stopping_ = false;
    }
    changed_.notify_all();
    if (callback_) {
        callback_(LinkState::Waiting, std::move(detail));
    }
}

void EngineLink::supervise(const std::stop_token& stop)
{
    // Never earlier than this, so a refused token is offered at
    // refused_interval and not at poll_interval. The epoch is always in the
    // past, so the first attempt is not delayed.
    auto not_before = std::chrono::steady_clock::time_point{};
    idle_.grace = config_.idle_grace;

    publish(LinkState::Starting, "connecting to the engine");

    while (!stop.stop_requested()) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool connected = client_ != nullptr;
        const bool ours = launched_;
        lock.unlock();

        if (connected) {
            // core/rpc/client.cpp keeps its event loop through a lost
            // connection and fails every later call, so this probe is the one
            // place that finds out. The Client is destroyed before another is
            // made: its loop thread is joined by that destructor and nothing
            // else.
            // lease_status, not running: the link observes, and an observer
            // may ask only info, listSources and leaseStatus.
            auto alive = client_->lease_status();
            if (!alive) {
                std::unique_ptr<rpc::Client> gone;
                lock.lock();
                gone = std::move(client_);
                // Free at the last reading means no phone held it: an engine
                // that died then is not one anybody is waiting on, so it is
                // not started again until a phone asks.
                if (idle_.free_since &&
                    (!last_demand_ ||
                     std::chrono::steady_clock::now() - *last_demand_ >= config_.idle_grace)) {
                    demanded_ = false;
                }
                lock.unlock();
                gone.reset();
                idle_.reset();
                publish(LinkState::Waiting, "the engine went away: " + alive.error().message);
                continue;
            }
            if (config_.on_demand) {
                // The decision and stopping_ under one lock with the claims'
                // last_demand_: see stopping_.
                lock.lock();
                const bool idle =
                    idle_.observe(alive->held, std::chrono::steady_clock::now(), last_demand_);
                if (idle) {
                    // The ask is over. Somebody else's engine is theirs to
                    // stop, but if it goes away now the link does not start
                    // one of its own for a phone that has left.
                    demanded_ = false;
                    stopping_ = ours;
                }
                lock.unlock();
                if (idle && !ours) {
                    idle_.reset();
                } else if (idle) {
                    stop_own_engine();
                    std::fprintf(stderr,
                                 "revenant-webgate: stopped the engine it started: no phone or "
                                 "other client has held it for %.1f s; the next phone starts it "
                                 "again\n",
                                 static_cast<double>(config_.idle_grace.count()) / 1000.0);
                    continue;
                }
            }
        } else if (std::chrono::steady_clock::now() >= not_before) {
            // Observe, so an idle gateway holds no lease and the desktop can
            // start while it runs. Phones claim on their own connections.
            Expected<std::unique_ptr<rpc::Client>> connection =
                new_session(rpc::LoginMode::Observe, kLinkHolder);
            if (connection) {
                lock.lock();
                client_ = std::move(*connection);
                lock.unlock();
                publish(LinkState::Up, "connected to the engine");
                continue;
            }

            const Error failure = connection.error();
            if (failure.category == ErrorCategory::Unauthenticated) {
                not_before = std::chrono::steady_clock::now() + config_.refused_interval;
                publish(LinkState::Refused, failure.message);
            } else {
                not_before = {};
                const std::optional<std::string> launch_note = maybe_launch();
                lock.lock();
                const bool coming_up = launched_ && !process_->exited();
                const bool asked = demanded_;
                if (asked && !coming_up) {
                    // Nothing is going to answer this ask: no engine to start,
                    // or ours died on the way up. The waiting phone gets the
                    // connect error now rather than at demand_wait.
                    demand_failed_ = true;
                }
                lock.unlock();
                if (asked && !coming_up) {
                    changed_.notify_all();
                }
                if (coming_up) {
                    publish(LinkState::Starting, "waiting for the engine to start");
                } else if (config_.on_demand && !asked && !launch_note) {
                    publish(LinkState::Waiting, "the engine starts when a phone connects");
                } else {
                    publish(LinkState::Waiting, launch_note ? *launch_note : failure.message);
                }
            }
        }

        // Woken early by a stop request, which is what lets the destructor
        // join without waiting out the interval, and by a phone's demand.
        lock.lock();
        wake_.wait_for(lock, stop, config_.poll_interval, [this] { return poked_; });
        poked_ = false;
    }
}

}  // namespace revenant::webgate
