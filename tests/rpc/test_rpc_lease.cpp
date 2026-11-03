// The engine's lease: one user at a time, with takeover.
//
// docs/superpowers/specs/2026-11-03-single-user-engine-design.md is the rule
// these cases pin. Every client here is a real connection: connect_as logs in
// with a mode and a holder, and destroying a Client closes its connection,
// which is the same event to the engine as a crashed process.
//
// WHY THE WAITS POLL. A connection closing is noticed on the server's loop
// after the client's destructor has returned, so a case reads leaseStatus or
// the engine's receiver list until the answer changes or a deadline passes.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

constexpr auto kWait = std::chrono::seconds(10);

void bring_up(Harness& harness) {
    HarnessOptions options;
    options.connect_client = false;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

[[nodiscard]] std::unique_ptr<rpc::Client> must_connect(Harness& harness, rpc::LoginMode mode,
                                                        std::string_view holder) {
    auto connected = harness.connect_as(mode, holder);
    INFO(test::message_of(connected));
    REQUIRE(connected.has_value());
    return std::move(*connected);
}

[[nodiscard]] rpc::VrxParams receiver_at(std::int64_t center) {
    return rpc::VrxParams{.center = center, .bandwidth = 12'000, .demod = rpc::Demod::Nfm};
}

[[nodiscard]] bool listed(const std::vector<std::uint64_t>& ids, std::uint64_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Polls until `done` holds or the deadline passes, and says which.
[[nodiscard]] bool eventually(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

[[nodiscard]] bool lease_free(rpc::Client& observer) {
    auto status = observer.lease_status();
    return status.has_value() && !status->held;
}

[[nodiscard]] bool engine_lists(Harness& harness, std::uint64_t id) {
    const auto ids = harness.engine().vrx_ids();
    return std::any_of(ids.begin(), ids.end(),
                       [id](const auto& vrx) { return vrx.value == id; });
}

// ISO 8601 local time with an offset, to the second.
const std::regex kSince(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2})");

}  // namespace

TEST_CASE("a claim takes a free lease and an observer sees who holds it", "[gpu][rpc][lease]") {
    REVENANT_NEEDS_GPU();
    Harness harness;
    bring_up(harness);

    auto observer = must_connect(harness, rpc::LoginMode::Observe, "probe");
    auto before = observer->lease_status();
    INFO(test::message_of(before));
    REQUIRE(before.has_value());
    CHECK_FALSE(before->held);
    CHECK(before->since.empty());

    auto desk = must_connect(harness, rpc::LoginMode::Claim, "Revenant desktop on TEST");
    auto after = observer->lease_status();
    INFO(test::message_of(after));
    REQUIRE(after.has_value());
    CHECK(after->held);
    CHECK(after->holder == "Revenant desktop on TEST");
    CHECK(std::regex_match(after->since, kSince));

    // The holder may ask too.
    auto own = desk->lease_status();
    REQUIRE(own.has_value());
    CHECK(own->holder == "Revenant desktop on TEST");
}

TEST_CASE("a claim against a held lease is refused naming the holder and the time",
          "[gpu][rpc][lease]") {
    REVENANT_NEEDS_GPU();
    Harness harness;
    bring_up(harness);

    auto desk = must_connect(harness, rpc::LoginMode::Claim, "Revenant desktop on TEST");
    auto held = desk->lease_status();
    REQUIRE(held.has_value());

    auto refused = harness.connect_as(rpc::LoginMode::Claim, "phone 10.1.10.57 via webgate");
    REQUIRE_FALSE(refused.has_value());
    const std::string text = refused.error().message;
    INFO(text);
    CHECK(text.find("in use: Revenant desktop on TEST since " + held->since) != std::string::npos);

    // Not a bad token: the holder leaving makes the same login succeed.
    CHECK(refused.error().category != ErrorCategory::Unauthenticated);
}

TEST_CASE("a login with no mode is a claim by an unknown client", "[gpu][rpc][lease]") {
    REVENANT_NEEDS_GPU();
    Harness harness;
    bring_up(harness);

    // The three-argument connect sends the schema defaults, which is the wire
    // an older client that knows neither field produces.
    auto old = harness.connect_another();
    INFO(test::message_of(old));
    REQUIRE(old.has_value());

    auto observer = must_connect(harness, rpc::LoginMode::Observe, "probe");
    auto status = observer->lease_status();
    REQUIRE(status.has_value());
    CHECK(status->held);
    CHECK(status->holder == "unknown client");

    // And an older client is refused cleanly while somebody else holds it.
    old->reset();
    REQUIRE(eventually([&] { return lease_free(*observer); }));
    auto phone = must_connect(harness, rpc::LoginMode::Claim, "phone via webgate");
    auto refused = harness.connect_another();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("in use: phone via webgate since ") != std::string::npos);
}

TEST_CASE("an observer may read the engine and nothing else", "[gpu][rpc][lease]") {
    REVENANT_NEEDS_GPU();
    Harness harness;
    bring_up(harness);

    auto observer = must_connect(harness, rpc::LoginMode::Observe, "probe");
    CHECK(observer->info().has_value());
    CHECK(observer->list_sources().has_value());
    CHECK(observer->lease_status().has_value());

    auto ids = observer->vrx_ids();
    REQUIRE_FALSE(ids.has_value());
    CHECK(ids.error().message.find("observing only") != std::string::npos);

    auto added = observer->add_vrx(receiver_at(131'072));
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error().message.find("observing only") != std::string::npos);

    auto running = observer->running();
    REQUIRE_FALSE(running.has_value());
    CHECK(running.error().message.find("observing only") != std::string::npos);

    // Observing takes nothing, so a claim still succeeds beside it.
    auto status = observer->lease_status();
    REQUIRE(status.has_value());
    CHECK_FALSE(status->held);
    auto desk = must_connect(harness, rpc::LoginMode::Claim, "desk");
    CHECK(desk->vrx_ids().has_value());
}

TEST_CASE("a takeover evicts the holder, tells it who, and keeps its receivers",
          "[gpu][rpc][lease]") {
    REVENANT_NEEDS_GPU();
    Harness harness;
    bring_up(harness);

    auto desk = must_connect(harness, rpc::LoginMode::Claim, "desk");
    auto made = desk->add_vrx(receiver_at(131'072));
    INFO(test::message_of(made));
    REQUIRE(made.has_value());

    std::mutex lock;
    std::optional<std::string> lost_to;
    desk->on_lease_lost([&](const std::string& holder) {
        const std::scoped_lock held(lock);
        lost_to = holder;
    });
    REQUIRE(desk->subscribe_vrx_events([](const rpc::VrxEvent&) {}, [](const std::string&) {})
                .has_value());

    auto phone = must_connect(harness, rpc::LoginMode::TakeOver, "phone via webgate");

    REQUIRE(eventually([&] {
        const std::scoped_lock held(lock);
        return lost_to.has_value();
    }));
    {
        const std::scoped_lock held(lock);
        CHECK(*lost_to == "phone via webgate");
    }

    // The old holder can do nothing more: refused while its connection is
    // still open, and disconnected once the engine closes it.
    auto after = desk->vrx_ids();
    CHECK_FALSE(after.has_value());

    // The new holder inherits the radio as it was, and controls it.
    auto ids = phone->vrx_ids();
    INFO(test::message_of(ids));
    REQUIRE(ids.has_value());
    CHECK(listed(*ids, *made));
    CHECK(phone->set_vrx_params(*made, receiver_at(140'000)).has_value());

    auto status = phone->lease_status();
    REQUIRE(status.has_value());
    CHECK(status->holder == "phone via webgate");

    // The evicted desk's connection closing must not end the new lease or
    // take the receiver with it.
    desk.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto still = phone->vrx_ids();
    REQUIRE(still.has_value());
    CHECK(listed(*still, *made));
    CHECK(phone->remove_vrx(*made).has_value());
}

TEST_CASE("the lease ends when its connection closes, and a crash counts", "[gpu][rpc][lease]") {
    REVENANT_NEEDS_GPU();
    Harness harness;
    bring_up(harness);

    auto observer = must_connect(harness, rpc::LoginMode::Observe, "probe");
    auto desk = must_connect(harness, rpc::LoginMode::Claim, "desk");
    desk.reset();
    REQUIRE(eventually([&] { return lease_free(*observer); }));

    auto phone = harness.connect_as(rpc::LoginMode::Claim, "phone");
    INFO(test::message_of(phone));
    CHECK(phone.has_value());
}

TEST_CASE("the lease ends with the holder's last claiming session, not its first",
          "[gpu][rpc][lease]") {
    REVENANT_NEEDS_GPU();
    Harness harness;
    bring_up(harness);

    auto observer = must_connect(harness, rpc::LoginMode::Observe, "probe");
    auto first = must_connect(harness, rpc::LoginMode::Claim, "gateway");
    auto second = must_connect(harness, rpc::LoginMode::Claim, "gateway");

    auto made = first->add_vrx(receiver_at(131'072));
    REQUIRE(made.has_value());

    first.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto held = observer->lease_status();
    REQUIRE(held.has_value());
    CHECK(held->held);
    CHECK(held->holder == "gateway");

    // The receiver the dropped session opened stays with the lease.
    auto ids = second->vrx_ids();
    REQUIRE(ids.has_value());
    CHECK(listed(*ids, *made));

    second.reset();
    REQUIRE(eventually([&] { return lease_free(*observer); }));
}

TEST_CASE("any session of the holder controls a receiver another session opened",
          "[gpu][rpc][lease]") {
    REVENANT_NEEDS_GPU();
    HarnessOptions options;
    options.connect_client = false;
    options.samples = 4'800'064;
    options.pace = 1.0;
    Harness harness;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
    REQUIRE(harness.start_engine().has_value());

    auto opener = must_connect(harness, rpc::LoginMode::Claim, "desk");
    auto other = must_connect(harness, rpc::LoginMode::Claim, "desk");

    auto made = opener->add_vrx(receiver_at(131'072));
    REQUIRE(made.has_value());

    CHECK(other->set_vrx_params(*made, receiver_at(140'000)).has_value());
    auto status = other->vrx_status(*made);
    REQUIRE(status.has_value());
    CHECK(status->params.center == 140'000);

    std::mutex lock;
    std::size_t chunks = 0;
    auto subscribed = other->subscribe_audio(
        *made, 0,
        [&](const rpc::AudioChunk&) {
            const std::scoped_lock held(lock);
            ++chunks;
        },
        [](const std::string&) {});
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());
    CHECK(eventually([&] {
        const std::scoped_lock held(lock);
        return chunks > 0;
    }));
    other->unsubscribe_audio(*made);

    CHECK(other->remove_vrx(*made).has_value());
    auto ids = opener->vrx_ids();
    REQUIRE(ids.has_value());
    CHECK_FALSE(listed(*ids, *made));

    CHECK(harness.stop_engine().has_value());
}

TEST_CASE("receivers clients opened go when the lease ends without a takeover, kept ones stay",
          "[gpu][rpc][lease]") {
    REVENANT_NEEDS_GPU();
    Harness harness;
    bring_up(harness);

    auto observer = must_connect(harness, rpc::LoginMode::Observe, "probe");
    std::uint64_t ordinary = 0;
    std::uint64_t kept = 0;
    {
        auto desk = must_connect(harness, rpc::LoginMode::Claim, "desk");
        auto one = desk->add_vrx(receiver_at(131'072));
        auto two = desk->add_vrx(receiver_at(-262'144), rpc::VrxLifetime::Kept);
        REQUIRE(one.has_value());
        REQUIRE(two.has_value());
        ordinary = *one;
        kept = *two;
    }

    REQUIRE(eventually([&] { return lease_free(*observer); }));
    CHECK(eventually([&] { return !engine_lists(harness, ordinary); }));
    CHECK(engine_lists(harness, kept));
}
