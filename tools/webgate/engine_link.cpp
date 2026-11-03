#include "tools/webgate/engine_link.h"

#include <span>
#include <system_error>
#include <utility>

#include "core/launch/engine_start.h"

namespace revenant::webgate {

EngineLink::EngineLink(EngineLinkConfig config) : config_(std::move(config)) {}

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
    // The Client goes before the launcher (a member, destroyed after this
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

Expected<std::unique_ptr<rpc::Client>> EngineLink::new_session()
{
    auto token = resolve_token();
    if (!token) {
        return std::unexpected(token.error());
    }
    return rpc::Client::connect(config_.address, config_.port,
                                std::span<const std::uint8_t>(token->data(), token->size()));
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
    // Outside the lock, so a callback that asks the link a question does not
    // deadlock on it.
    if (callback_) {
        callback_(state, std::move(detail));
    }
}

std::optional<std::string> EngineLink::maybe_launch()
{
    if (launched_) {
        if (!launcher_.exited()) {
            return std::nullopt;  // Ours, still coming up.
        }
        if (std::chrono::steady_clock::now() < launch_not_before_) {
            return launch::engine_exit_sentence(launcher_.exited()->code,
                                                launcher_.exited()->tail);
        }
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
    std::string refusal = launcher_.start(config_.engine_exe, config_.port,
                                          resolved_token_path(), config_.capture);
    if (!refusal.empty()) {
        return refusal;
    }
    launched_ = true;
    return std::nullopt;
}

void EngineLink::supervise(const std::stop_token& stop)
{
    // Never earlier than this, so a refused token is offered at
    // refused_interval and not at poll_interval. The epoch is always in the
    // past, so the first attempt is not delayed.
    auto not_before = std::chrono::steady_clock::time_point{};

    publish(LinkState::Starting, "connecting to the engine");

    while (!stop.stop_requested()) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool connected = client_ != nullptr;
        lock.unlock();

        if (connected) {
            // core/rpc/client.cpp keeps its event loop through a lost
            // connection and fails every later call, so this probe is the one
            // place that finds out. The Client is destroyed before another is
            // made: its loop thread is joined by that destructor and nothing
            // else.
            if (auto alive = client_->running(); !alive) {
                std::unique_ptr<rpc::Client> gone;
                lock.lock();
                gone = std::move(client_);
                lock.unlock();
                gone.reset();
                publish(LinkState::Waiting, "the engine went away: " + alive.error().message);
                continue;
            }
        } else if (std::chrono::steady_clock::now() >= not_before) {
            Expected<std::unique_ptr<rpc::Client>> connection = new_session();
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
                if (launched_ && !launcher_.exited()) {
                    publish(LinkState::Starting, "waiting for the engine to start");
                } else {
                    publish(LinkState::Waiting, launch_note ? *launch_note : failure.message);
                }
            }
        }

        // Woken early by a stop request, which is what lets the destructor
        // join without waiting out the interval.
        lock.lock();
        wake_.wait_for(lock, stop, config_.poll_interval, [] { return false; });
    }
}

}  // namespace revenant::webgate
