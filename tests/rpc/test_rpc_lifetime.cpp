// Receivers belong to the lease, not to the session that created them.
//
// THE DECISION THESE CASES PIN
//
// From 2026-09-22 a receiver was removed when the session that created it
// ended, unless addVrx was asked to keep it. The owner replaced that on
// 2026-11-03 with the single-user lease
// (docs/superpowers/specs/2026-11-03-single-user-engine-design.md): every
// session of the lease holder shares every receiver, a session leaving takes
// nothing with it while the holder has another, and the receivers clients
// opened without keep go when the lease ends without a takeover.
// tests/rpc/test_rpc_lease.cpp has the lease itself; these cases are about
// what happens to receivers.
//
// A session ends when its capability is released, and the only way these
// cases can make that happen is the way a real client does: destroy the
// rpc::Client, which closes the connection. That is also exactly what a
// crashed window looks like from the server.
//
// WHY EVERY WAIT HERE POLLS
//
// The removal happens on the server's event loop when it notices the socket
// has closed, and the client's destructor returns before that. So a case reads
// the receiver list until the answer changes or a deadline passes, and reports
// what it last saw.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
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

// Long enough for a loaded CI machine to notice a closed socket, and far
// longer than it takes on an idle one.
constexpr int kReapWaitMs = 10'000;

// How long a case waits to be sure something did NOT happen. A reap that was
// going to come would have come well inside this on loopback.
constexpr auto kQuiet = std::chrono::milliseconds(500);

void bring_up(Harness& harness, const HarnessOptions& options) {
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

[[nodiscard]] rpc::VrxParams receiver_at(std::int64_t center) {
    return rpc::VrxParams{.center = center, .bandwidth = 12'000, .demod = rpc::Demod::Nfm};
}

[[nodiscard]] bool contains(const std::vector<std::uint64_t>& ids, std::uint64_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

[[nodiscard]] bool engine_lists(Harness& harness, std::uint64_t id) {
    const auto ids = harness.engine().vrx_ids();
    return std::any_of(ids.begin(), ids.end(),
                       [id](const auto& vrx) { return vrx.value == id; });
}

// Polls the engine until `gone` is no longer listed, and says whether it went.
[[nodiscard]] bool wait_until_gone(Harness& harness, std::uint64_t gone) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kReapWaitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!engine_lists(harness, gone)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !engine_lists(harness, gone);
}

}  // namespace

TEST_CASE("a session that disconnects while its holder has another leaves its receivers",
          "[gpu][rpc][lifetime]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    std::uint64_t first = 0;
    std::uint64_t second = 0;
    {
        auto leaver = harness.connect_another();
        INFO(test::message_of(leaver));
        REQUIRE(leaver.has_value());

        auto one = (*leaver)->add_vrx(receiver_at(131'072));
        auto two = (*leaver)->add_vrx(receiver_at(-262'144));
        INFO(test::message_of(one));
        INFO(test::message_of(two));
        REQUIRE(one.has_value());
        REQUIRE(two.has_value());
        first = *one;
        second = *two;

        // Who opened it is still reported, as provenance. Asked from both
        // ends, because owned_by_caller is computed per caller.
        auto mine = (*leaver)->vrx_status(first);
        auto theirs = harness.client().vrx_status(first);
        REQUIRE(mine.has_value());
        REQUIRE(theirs.has_value());
        CHECK(mine->owned_by_caller);
        CHECK_FALSE(theirs->owned_by_caller);
        CHECK(theirs->creator_session == mine->creator_session);

        // Destroyed without removing anything, which is a crash as far as the
        // server can tell.
    }

    std::this_thread::sleep_for(kQuiet);
    auto listed = harness.client().vrx_ids();
    INFO(test::message_of(listed));
    REQUIRE(listed.has_value());
    CHECK(contains(*listed, first));
    CHECK(contains(*listed, second));

    // And the survivor controls them as its own.
    CHECK(harness.client().set_vrx_params(first, receiver_at(140'000)).has_value());
    CHECK(harness.client().remove_vrx(first).has_value());
    CHECK(harness.client().remove_vrx(second).has_value());
}

TEST_CASE("a kept receiver outlives the lease that created it", "[gpu][rpc][lifetime]") {
    REVENANT_NEEDS_GPU();

    HarnessOptions options;
    options.connect_client = false;
    Harness harness;
    bring_up(harness, options);

    std::uint64_t kept = 0;
    std::uint64_t control = 0;
    std::uint64_t creator = 0;
    {
        auto recorder = harness.connect_another();
        INFO(test::message_of(recorder));
        REQUIRE(recorder.has_value());

        auto made = (*recorder)->add_vrx(receiver_at(131'072), rpc::VrxLifetime::Kept);
        INFO(test::message_of(made));
        REQUIRE(made.has_value());
        kept = *made;

        // THE CONTROL ARM. Without a receiver that is NOT kept, the survivor
        // below would pass on a server that reaped nothing at all.
        auto ordinary = (*recorder)->add_vrx(receiver_at(-262'144));
        INFO(test::message_of(ordinary));
        REQUIRE(ordinary.has_value());
        control = *ordinary;

        auto status = (*recorder)->vrx_status(kept);
        REQUIRE(status.has_value());
        CHECK(status->kept);
        creator = status->creator_session;
    }

    REQUIRE(wait_until_gone(harness, control));
    CHECK(engine_lists(harness, kept));

    // The next holder finds it, names the session that made it, and may
    // remove it.
    auto next = harness.connect_another();
    INFO(test::message_of(next));
    REQUIRE(next.has_value());
    auto status = (*next)->vrx_status(kept);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());
    CHECK(status->kept);
    CHECK(status->creator_session == creator);
    CHECK_FALSE(status->owned_by_caller);
    CHECK((*next)->remove_vrx(kept).has_value());
    CHECK(harness.engine().vrx_ids().empty());
}
