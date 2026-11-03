// EngineLink against the in-process engine of tests/rpc/rpc_fixture.h.
//
// Every wait here is on a condition with a generous ceiling rather than a
// sleep, because the supervisor polls and a loaded CI box can be slow; the
// ceiling is only ever reached by a failure.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/token.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tools/webgate/engine_link.h"

namespace {

using namespace std::chrono_literals;
using revenant::webgate::EngineLink;
using revenant::webgate::EngineLinkConfig;
using revenant::webgate::LinkState;

// Records every state the link reports, in order, and lets a case wait for one.
class StateLog {
public:
    void record(LinkState state)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            seen_.push_back(state);
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool wait_for(LinkState wanted, std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, limit, [&] {
            return std::find(seen_.begin(), seen_.end(), wanted) != seen_.end();
        });
    }

    [[nodiscard]] bool saw(LinkState wanted)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return std::find(seen_.begin(), seen_.end(), wanted) != seen_.end();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<LinkState> seen_;
};

[[nodiscard]] EngineLinkConfig config_for(std::uint16_t port)
{
    EngineLinkConfig config;
    config.port = port;
    config.token = revenant::test::test_token();
    config.poll_interval = 50ms;
    return config;
}

// A loopback port with nothing on it: bound to learn the number, then closed.
// Another process could take it in between, which is a risk the case accepts
// over hard-coding a port.
[[nodiscard]] std::uint16_t unused_port()
{
    WSADATA data{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    const SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(probe != INVALID_SOCKET);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    at.sin_port = 0;
    REQUIRE(bind(probe, reinterpret_cast<const sockaddr*>(&at), sizeof at) == 0);
    int length = sizeof at;
    REQUIRE(getsockname(probe, reinterpret_cast<sockaddr*>(&at), &length) == 0);
    const std::uint16_t port = ntohs(at.sin_port);
    closesocket(probe);
    WSACleanup();
    return port;
}

[[nodiscard]] revenant::rpc::VrxParams receiver_at(std::int64_t center)
{
    return revenant::rpc::VrxParams{
        .center = center, .bandwidth = 12'000, .demod = revenant::rpc::Demod::Nfm};
}

}  // namespace

TEST_CASE("reaches Up against a running engine", "[gpu][webgate][link]")
{
    REVENANT_NEEDS_GPU();

    revenant::test::Harness harness;
    const auto ready = harness.open(revenant::test::HarnessOptions{});
    INFO(revenant::test::message_of(ready));
    REQUIRE(ready.has_value());

    StateLog log;
    EngineLink link(config_for(harness.port()));
    link.on_state([&log](LinkState state, const std::string&) { log.record(state); });
    link.start();

    REQUIRE(log.wait_for(LinkState::Up, 30s));
    REQUIRE(link.state() == LinkState::Up);
    REQUIRE(link.client() != nullptr);
    // A call through it, because Up that cannot talk is not Up.
    const auto info = link.client()->info();
    INFO(revenant::test::message_of(info));
    REQUIRE(info.has_value());
}

TEST_CASE("reports Waiting with nothing on the port and no engine beside it",
          "[webgate][link]")
{
    StateLog log;
    EngineLink link(config_for(unused_port()));
    link.on_state([&log](LinkState state, const std::string&) { log.record(state); });
    link.start();

    REQUIRE(log.wait_for(LinkState::Waiting, 30s));
    REQUIRE(link.client() == nullptr);
    // Refused is for an engine that answered; nothing answered.
    REQUIRE_FALSE(log.saw(LinkState::Refused));
    REQUIRE_FALSE(log.saw(LinkState::Up));

    const auto session = link.new_session();
    REQUIRE_FALSE(session.has_value());
    REQUIRE(session.error().category == revenant::ErrorCategory::Unreachable);
}

TEST_CASE("wrong token is Refused, not Waiting", "[gpu][webgate][link]")
{
    REVENANT_NEEDS_GPU();

    revenant::test::Harness harness;
    const auto ready = harness.open(revenant::test::HarnessOptions{});
    INFO(revenant::test::message_of(ready));
    REQUIRE(ready.has_value());

    EngineLinkConfig config = config_for(harness.port());
    config.token = revenant::test::wrong_test_token();

    StateLog log;
    EngineLink link(config);
    link.on_state([&log](LinkState state, const std::string&) { log.record(state); });
    link.start();

    REQUIRE(log.wait_for(LinkState::Refused, 30s));
    REQUIRE(link.client() == nullptr);
    // Held off at refused_interval, so a second pass has not had time to
    // report anything else, and in any case an answering engine is never
    // Waiting.
    REQUIRE_FALSE(log.saw(LinkState::Waiting));
    REQUIRE_FALSE(log.saw(LinkState::Up));
}

TEST_CASE("a refused token is retried at the backoff and picks up a fixed file",
          "[gpu][webgate][link]")
{
    REVENANT_NEEDS_GPU();

    revenant::test::Harness harness;
    const auto ready = harness.open(revenant::test::HarnessOptions{});
    INFO(revenant::test::message_of(ready));
    REQUIRE(ready.has_value());

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("revenant-webgate-link-" + std::to_string(::GetCurrentProcessId()) + ".token");
    const auto remove_file = [&path] {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    };
    remove_file();

    const auto wrote_wrong = revenant::rpc::write_private_file(
        path.string(), revenant::rpc::token_to_hex(revenant::test::wrong_test_token()), true);
    INFO(revenant::test::message_of(wrote_wrong));
    REQUIRE(wrote_wrong.has_value());

    EngineLinkConfig config = config_for(harness.port());
    config.token.reset();
    config.token_path = path;
    config.refused_interval = 300ms;

    StateLog log;
    EngineLink link(config);
    link.on_state([&log](LinkState state, const std::string&) { log.record(state); });
    link.start();

    REQUIRE(log.wait_for(LinkState::Refused, 30s));

    // The right token written after the link started: resolved per attempt, so
    // the next one after the backoff gets in.
    const auto wrote_right = revenant::rpc::write_private_file(
        path.string(), revenant::rpc::token_to_hex(revenant::test::test_token()), true);
    INFO(revenant::test::message_of(wrote_right));
    REQUIRE(wrote_right.has_value());

    const bool came_up = log.wait_for(LinkState::Up, 30s);
    remove_file();
    REQUIRE(came_up);
}

TEST_CASE("new_session returns independent sessions", "[gpu][webgate][link]")
{
    REVENANT_NEEDS_GPU();

    revenant::test::Harness harness;
    const auto ready = harness.open(revenant::test::HarnessOptions{});
    INFO(revenant::test::message_of(ready));
    REQUIRE(ready.has_value());

    EngineLink link(config_for(harness.port()));
    StateLog log;
    link.on_state([&log](LinkState state, const std::string&) { log.record(state); });
    link.start();
    REQUIRE(log.wait_for(LinkState::Up, 30s));

    auto first = link.new_session();
    auto second = link.new_session();
    INFO(revenant::test::message_of(first));
    INFO(revenant::test::message_of(second));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->get() != second->get());
    REQUIRE(first->get() != link.client());

    const auto made = (*first)->add_vrx(receiver_at(131'072));
    INFO(revenant::test::message_of(made));
    REQUIRE(made.has_value());

    // vrx_ids lists the engine's receivers for every caller (an observer sees
    // another session's), so independence shows in who owns the receiver, and
    // in what happens to it when its session goes.
    const auto mine = (*first)->vrx_status(*made);
    const auto theirs = (*second)->vrx_status(*made);
    INFO(revenant::test::message_of(mine));
    INFO(revenant::test::message_of(theirs));
    REQUIRE(mine.has_value());
    REQUIRE(theirs.has_value());
    REQUIRE(mine->owned_by_caller);
    REQUIRE_FALSE(theirs->owned_by_caller);

    const auto kept = (*second)->add_vrx(receiver_at(-262'144));
    INFO(revenant::test::message_of(kept));
    REQUIRE(kept.has_value());

    // The first phone goes away: its receiver is reaped, the second's stays.
    first->reset();
    bool reaped = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto ids = (*second)->vrx_ids();
        REQUIRE(ids.has_value());
        if (std::find(ids->begin(), ids->end(), *made) == ids->end()) {
            reaped = true;
            REQUIRE(std::find(ids->begin(), ids->end(), *kept) != ids->end());
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(reaped);

}
