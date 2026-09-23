// Receivers belong to the session that created them.
//
// THE DECISION THESE CASES PIN
//
// Recorded by the owner on 2026-09-22: a receiver is removed when the session
// that created it ends, unless addVrx was asked to keep it. Until then a
// receiver outlived its client and nothing reaped one whose client died, which
// docs/rpc.md measured as a `taskkill /F` leaving its receiver behind until the
// engine was restarted.
//
// A session ends when its capability is released, and the only way these
// cases can make that happen is the way a real client does: destroy the
// rpc::Client, which closes the connection. That is also exactly what a
// crashed window looks like from the server, because the capability goes with
// the connection either way and there is no goodbye message on this wire to
// miss.
//
// WHY EVERY WAIT HERE POLLS
//
// The removal happens on the server's event loop when it notices the socket
// has closed, and the client's destructor returns before that. So a case reads
// vrxIds from a surviving client until the answer changes or a deadline
// passes, and reports what it last saw. Each wait also records how long the
// reap took, which is printed rather than asserted: the number is a property of
// the loopback stack and not of this code, and the only claim made about it is
// that it is finite.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
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

void bring_up(Harness& harness, const HarnessOptions& options) {
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

// Two receivers the scene's grid carries comfortably, at distinct offsets so a
// case can tell them apart by id and by centre.
[[nodiscard]] rpc::VrxParams receiver_at(std::int64_t center) {
    return rpc::VrxParams{.center = center, .bandwidth = 12'000, .demod = rpc::Demod::Nfm};
}

[[nodiscard]] bool contains(const std::vector<std::uint64_t>& ids, std::uint64_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

struct Reaped {
    std::vector<std::uint64_t> ids;
    std::chrono::milliseconds took{0};
    bool gone = false;
};

// Polls vrxIds from `observer` until `gone` is no longer listed, and hands back
// the last list either way.
[[nodiscard]] Reaped wait_until_gone(rpc::Client& observer, std::uint64_t gone) {
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(kReapWaitMs);
    Reaped out;
    for (;;) {
        auto ids = observer.vrx_ids();
        if (ids) {
            out.ids = std::move(*ids);
            if (!contains(out.ids, gone)) {
                out.gone = true;
                break;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    out.took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return out;
}

// An audio subscriber's ending, kept so a case can ask whether the server
// said why the stream stopped.
class Ending {
public:
    void record(const std::string& reason) {
        const std::lock_guard<std::mutex> held(lock_);
        ended_ = true;
        reason_ = reason;
    }
    [[nodiscard]] bool ended() const {
        const std::lock_guard<std::mutex> held(lock_);
        return ended_;
    }
    [[nodiscard]] std::string reason() const {
        const std::lock_guard<std::mutex> held(lock_);
        return reason_;
    }

private:
    mutable std::mutex lock_;
    bool ended_ = false;
    std::string reason_;
};

}  // namespace

TEST_CASE("a session that disconnects leaves no receivers behind", "[gpu][rpc][lifetime]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t creator = 0;
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

        // The creator sees its own receiver as its own, and the observer sees
        // the same creator number as somebody else's. Asked from both ends,
        // because owned_by_caller is computed per caller and a server that
        // answered true to everybody would pass a check made from one side.
        auto mine = (*leaver)->vrx_status(first);
        auto theirs = harness.client().vrx_status(first);
        INFO(test::message_of(mine));
        INFO(test::message_of(theirs));
        REQUIRE(mine.has_value());
        REQUIRE(theirs.has_value());
        CHECK(mine->owned_by_caller);
        CHECK_FALSE(mine->kept);
        CHECK(mine->creator_session != 0);
        CHECK_FALSE(theirs->owned_by_caller);
        CHECK(theirs->creator_session == mine->creator_session);
        creator = mine->creator_session;

        auto listed = harness.client().vrx_ids();
        REQUIRE(listed.has_value());
        CHECK(listed->size() == 2);

        // Destroyed without removing anything, which is a crash as far as the
        // server can tell.
    }

    const Reaped after_first = wait_until_gone(harness.client(), first);
    INFO(std::format("receiver {} still listed after {} ms; list has {}", first,
                     after_first.took.count(), after_first.ids.size()));
    REQUIRE(after_first.gone);

    const Reaped after_second = wait_until_gone(harness.client(), second);
    REQUIRE(after_second.gone);
    CHECK(after_second.ids.empty());
    WARN(std::format("session {} ended; its two receivers were gone {} ms after the client "
                     "was destroyed",
                     creator, after_first.took.count() + after_second.took.count()));

    // And removed in the engine, not merely hidden from the wire.
    CHECK(harness.engine().vrx_ids().empty());
}

TEST_CASE("a kept receiver outlives the session that created it", "[gpu][rpc][lifetime]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

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

        // THE CONTROL ARM. Without a receiver in the same session that is NOT
        // kept, the survivor below would pass on a server that reaped nothing
        // at all, which is the behaviour this change replaced.
        auto ordinary = (*recorder)->add_vrx(receiver_at(-262'144));
        INFO(test::message_of(ordinary));
        REQUIRE(ordinary.has_value());
        control = *ordinary;

        auto status = (*recorder)->vrx_status(kept);
        REQUIRE(status.has_value());
        CHECK(status->kept);
        CHECK(status->owned_by_caller);
        creator = status->creator_session;
    }

    const Reaped reaped = wait_until_gone(harness.client(), control);
    INFO(std::format("control receiver {} still listed after {} ms", control,
                     reaped.took.count()));
    REQUIRE(reaped.gone);
    CHECK(contains(reaped.ids, kept));

    // It still names the session that made it, which has ended, and it is
    // nobody's now: not the observer's, and not a receiver the observer could
    // mistake for its own.
    auto status = harness.client().vrx_status(kept);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());
    CHECK(status->kept);
    CHECK(status->creator_session == creator);
    CHECK_FALSE(status->owned_by_caller);

    // Kept means until somebody removes it, and anybody may.
    CHECK(harness.client().remove_vrx(kept).has_value());
    auto after = harness.client().vrx_ids();
    REQUIRE(after.has_value());
    CHECK(after->empty());
}

TEST_CASE("a session ending leaves every other session's receivers alone",
          "[gpu][rpc][lifetime]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // The harness client's own receiver, made first so its id is the lower
    // one and an off-by-one in the reap would reach it.
    auto staying = harness.client().add_vrx(receiver_at(131'072));
    INFO(test::message_of(staying));
    REQUIRE(staying.has_value());

    std::uint64_t leaving = 0;
    {
        auto other = harness.connect_another();
        INFO(test::message_of(other));
        REQUIRE(other.has_value());

        auto made = (*other)->add_vrx(receiver_at(-262'144));
        INFO(test::message_of(made));
        REQUIRE(made.has_value());
        leaving = *made;

        // Touching another session's receiver does not make it yours. The
        // leaving session retunes the staying one, which is allowed, and the
        // reap must still leave it where it is.
        rpc::VrxParams moved = receiver_at(140'000);
        CHECK((*other)->set_vrx_params(*staying, moved).has_value());
    }

    const Reaped reaped = wait_until_gone(harness.client(), leaving);
    INFO(std::format("receiver {} still listed after {} ms", leaving, reaped.took.count()));
    REQUIRE(reaped.gone);
    REQUIRE(reaped.ids.size() == 1);
    CHECK(reaped.ids.front() == *staying);

    auto status = harness.client().vrx_status(*staying);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());
    CHECK(status->owned_by_caller);
    CHECK(status->params.center == 140'000);
}

TEST_CASE("a listener on a reaped receiver is told the session that owned it ended",
          "[gpu][rpc][lifetime]") {
    REVENANT_NEEDS_GPU();

    // Running, because an audio subscription is only told anything once there
    // is a stream to end.
    HarnessOptions options;
    options.samples = 4'800'064;
    options.pace = 1.0;
    Harness harness;
    bring_up(harness, options);
    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    auto ending = std::make_shared<Ending>();
    std::uint64_t vrx = 0;
    {
        auto owner = harness.connect_another();
        INFO(test::message_of(owner));
        REQUIRE(owner.has_value());

        auto made = (*owner)->add_vrx(receiver_at(131'072));
        INFO(test::message_of(made));
        REQUIRE(made.has_value());
        vrx = *made;

        // The LISTENER is the surviving client, subscribed to a receiver it
        // did not create. That is the arrangement where the removal is news:
        // the owner knows it left, the listener does not.
        auto subscribed = harness.client().subscribe_audio(
            vrx, 0, [](const rpc::AudioChunk&) {},
            [ending](const std::string& reason) { ending->record(reason); });
        INFO(test::message_of(subscribed));
        REQUIRE(subscribed.has_value());
    }

    const Reaped reaped = wait_until_gone(harness.client(), vrx);
    REQUIRE(reaped.gone);

    // ended() travels on the listener's own connection and can land a moment
    // after the removal is visible in vrxIds.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ending->ended() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    INFO(ending->reason());
    REQUIRE(ending->ended());
    CHECK(ending->reason().find("session that created this receiver ended") !=
          std::string::npos);

    const Status stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    CHECK(stopped.has_value());
}
