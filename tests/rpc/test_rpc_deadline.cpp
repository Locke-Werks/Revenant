// rpc::Client's call deadlines, against an engine stand-in that stops
// answering on cue. No GPU: tests/rpc/stall_engine.h has no engine in it.

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/rpc/client.h"
#include "tests/rpc/stall_engine.h"

namespace {

using namespace std::chrono_literals;
using revenant::ErrorCategory;
namespace rpc = revenant::rpc;

[[nodiscard]] std::unique_ptr<rpc::Client> connect_to(const revenant::test::StallEngine& engine) {
    const std::array<std::uint8_t, rpc::kTokenBytes> token{};
    auto client = rpc::Client::connect("127.0.0.1", engine.port(), token);
    REQUIRE(client);
    return std::move(*client);
}

[[nodiscard]] double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Polled, because the cancellation reaches the stand-in over the socket after
// the caller has already been answered.
[[nodiscard]] bool eventually(const auto& condition) {
    for (int i = 0; i < 200; ++i) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}

}  // namespace

TEST_CASE("a call the engine never answers fails at its deadline and says so",
          "[rpc][deadline]") {
    revenant::test::StallEngine engine;
    auto client = connect_to(engine);
    REQUIRE(client->running());

    engine.answer(false);
    const auto start = std::chrono::steady_clock::now();
    {
        const rpc::CallDeadline scope(300ms);
        const auto running = client->running();
        REQUIRE_FALSE(running);
        CHECK(running.error().category == ErrorCategory::TimedOut);
        CHECK(rpc::timed_out(running.error()));
        CHECK(running.error().message == "running: the engine did not answer within 0.3 s");
        CHECK(scope.timed_out());
    }
    const double took = seconds_since(start);
    CHECK(took >= 0.25);
    CHECK(took < 2.0);

    // Given up on at the engine's end too: capnp sent the Finish.
    CHECK(eventually([&] { return engine.cancelled() >= 1; }));
}

TEST_CASE("a client whose call timed out is usable afterwards", "[rpc][deadline]") {
    revenant::test::StallEngine engine;
    auto client = connect_to(engine);

    engine.answer(false);
    {
        const rpc::CallDeadline scope(200ms);
        CHECK_FALSE(client->info());
        CHECK_FALSE(client->lease_status());
    }

    engine.answer(true);
    const auto before = engine.answered();
    const auto info = client->info();
    REQUIRE(info);
    const auto lease = client->lease_status();
    REQUIRE(lease);
    CHECK(lease->held);
    CHECK(engine.answered() == before + 2);
}

TEST_CASE("a poll scope waits once and fails the rest at once", "[rpc][deadline]") {
    revenant::test::StallEngine engine;
    auto client = connect_to(engine);
    auto other = connect_to(engine);

    engine.answer(false);
    const auto start = std::chrono::steady_clock::now();
    {
        const rpc::CallDeadline poll(300ms, true);
        CHECK_FALSE(client->info());
        CHECK_FALSE(client->running());
        // Any client: the scope belongs to the thread, and a stalled engine
        // is stalled for every connection to it.
        const auto skipped = other->info();
        REQUIRE_FALSE(skipped);
        CHECK(skipped.error().category == ErrorCategory::TimedOut);
        CHECK(skipped.error().message.find("skipped") != std::string::npos);
    }
    CHECK(seconds_since(start) < 1.0);
}

TEST_CASE("kOwnDeadline keeps the call's own deadline", "[rpc][deadline]") {
    revenant::test::StallEngine engine;
    auto client = connect_to(engine);
    engine.answer(false);

    const rpc::CallDeadline outer(200ms);
    const rpc::CallDeadline own(rpc::kOwnDeadline, true);
    REQUIRE(rpc::CallDeadline::current() == &own);
    const auto start = std::chrono::steady_clock::now();
    const auto running = client->running();
    REQUIRE_FALSE(running);
    // kCallDeadline, not the outer 200 ms.
    CHECK(running.error().message == "running: the engine did not answer within 5 s");
    CHECK(seconds_since(start) >= 4.5);
}

TEST_CASE("a call queued behind a stalled one times out on its own deadline",
          "[rpc][deadline]") {
    revenant::test::StallEngine engine;
    auto client = connect_to(engine);
    engine.answer(false);

    std::thread first([&] {
        const rpc::CallDeadline scope(1500ms);
        static_cast<void>(client->info());
    });
    std::this_thread::sleep_for(100ms);
    const auto start = std::chrono::steady_clock::now();
    {
        const rpc::CallDeadline scope(300ms);
        const auto queued = client->running();
        REQUIRE_FALSE(queued);
        CHECK(queued.error().category == ErrorCategory::TimedOut);
    }
    CHECK(seconds_since(start) < 1.0);
    first.join();
}

TEST_CASE("a timeout inside a nested scope marks the outer poll scope", "[rpc][deadline]") {
    revenant::test::StallEngine engine;
    auto client = connect_to(engine);
    engine.answer(false);

    const rpc::CallDeadline outer(200ms, true);
    {
        const rpc::CallDeadline inner(200ms);
        CHECK_FALSE(client->info());
    }
    CHECK(outer.timed_out());
    CHECK(rpc::CallDeadline::current() == &outer);
}
