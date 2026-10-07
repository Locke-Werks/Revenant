// Receiver events: Session.subscribeVrxEvents and the owner fields on
// VrxStatus.
//
// WHY THIS EXISTS
//
// Engine plugins open and close receivers on their own, a P25 trunk tracker
// one per call, and until 2026-10-06 a client had no way to hear about a
// receiver it did not create short of polling vrxIds, which misses one that
// lives for a second between two polls. So the client never showed a
// plugin's receivers and never played them. These cases pin the push stream
// that replaced the poll, from the client library's side of a real socket.
//
// WHAT THEY PIN
//
// The replay of what exists, then every change in order; the owner classified
// against the subscribing session, so the same receiver is thisSession to one
// client and otherSession to another; removals for every way a receiver goes,
// a closed source included, where the engine reports nothing one by one; and
// the RDS companion the server opens beside a stereo listener staying out of
// the stream, as it stays out of vrxIds. The plugin case is in
// test_rpc_engine_plugins.cpp, beside the fixture plugin it needs.
//
// HOW A NEGATIVE IS CHECKED
//
// "Nothing else was sent" cannot be waited for. Events reach one subscriber
// in the order they happened, so each case that claims an absence adds a
// FENCE receiver after the thing that must not appear and waits for the
// fence's added: anything the server was going to say before it has been
// said by then.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/engine/engine.h"
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

// Long enough for a loaded machine to notice a closed socket, which is the
// slowest thing any case here waits for.
constexpr int kEventWaitMs = 10'000;

void bring_up(Harness& harness, const HarnessOptions& options) {
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

[[nodiscard]] rpc::VrxParams receiver_at(std::int64_t center) {
    return rpc::VrxParams{.center = center, .bandwidth = 12'000, .demod = rpc::Demod::Nfm};
}

[[nodiscard]] const char* kind_name(rpc::VrxEventKind kind) {
    switch (kind) {
        case rpc::VrxEventKind::Added: return "added";
        case rpc::VrxEventKind::Changed: return "changed";
        case rpc::VrxEventKind::Removed: return "removed";
    }
    return "?";
}

// Everything one subscription was sent, in arrival order. The callbacks run
// on the client's loop thread and the case reads from its own, hence the lock.
class EventLog {
public:
    void record(const rpc::VrxEvent& event) {
        const std::lock_guard<std::mutex> held(lock_);
        events_.push_back(event);
    }
    void end(const std::string& reason) {
        const std::lock_guard<std::mutex> held(lock_);
        ended_ = true;
        reason_ = reason;
    }
    [[nodiscard]] std::vector<rpc::VrxEvent> events() const {
        const std::lock_guard<std::mutex> held(lock_);
        return events_;
    }
    [[nodiscard]] bool ended() const {
        const std::lock_guard<std::mutex> held(lock_);
        return ended_;
    }

    // Every event as one line each, for an INFO that says what did arrive
    // when a wait for something else failed.
    [[nodiscard]] std::string describe() const {
        std::string out;
        for (const rpc::VrxEvent& event : events()) {
            out += std::format("{} vrx {} owner {} '{}' reason '{}'\n", kind_name(event.kind),
                               event.vrx, static_cast<int>(event.owner), event.owner_name,
                               event.reason);
        }
        return out.empty() ? std::string("(no events)") : out;
    }

    // Polls until an event of `kind` about `vrx` has arrived, and hands back
    // a copy of it, or nothing at the deadline.
    [[nodiscard]] std::optional<rpc::VrxEvent> wait_for(rpc::VrxEventKind kind,
                                                        std::uint64_t vrx) const {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kEventWaitMs);
        for (;;) {
            for (const rpc::VrxEvent& event : events()) {
                if (event.kind == kind && event.vrx == vrx) {
                    return event;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

private:
    mutable std::mutex lock_;
    std::vector<rpc::VrxEvent> events_;
    bool ended_ = false;
    std::string reason_;
};

void subscribe(rpc::Client& client, const std::shared_ptr<EventLog>& log) {
    const Status subscribed = client.subscribe_vrx_events(
        [log](const rpc::VrxEvent& event) { log->record(event); },
        [log](const std::string& reason) { log->end(reason); });
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());
}

// The events about one receiver, in order.
[[nodiscard]] std::vector<rpc::VrxEvent> about(const std::vector<rpc::VrxEvent>& events,
                                               std::uint64_t vrx) {
    std::vector<rpc::VrxEvent> out;
    std::copy_if(events.begin(), events.end(), std::back_inserter(out),
                 [vrx](const rpc::VrxEvent& event) { return event.vrx == vrx; });
    return out;
}

}  // namespace

TEST_CASE("subscribing replays every receiver that already exists, with its owner",
          "[gpu][rpc][vrx-events]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto mine = harness.client().add_vrx(receiver_at(131'072));
    INFO(test::message_of(mine));
    REQUIRE(mine.has_value());

    // Another session's, kept alive for the whole case so it is a live
    // session's receiver and not a kept orphan.
    auto other = harness.connect_another();
    INFO(test::message_of(other));
    REQUIRE(other.has_value());
    auto theirs = (*other)->add_vrx(receiver_at(-262'144));
    INFO(test::message_of(theirs));
    REQUIRE(theirs.has_value());

    auto log = std::make_shared<EventLog>();
    subscribe(harness.client(), log);

    const auto own = log->wait_for(rpc::VrxEventKind::Added, *mine);
    const auto foreign = log->wait_for(rpc::VrxEventKind::Added, *theirs);
    INFO(log->describe());
    REQUIRE(own.has_value());
    REQUIRE(foreign.has_value());

    CHECK(own->owner == rpc::VrxOwnerKind::ThisSession);
    CHECK(own->owner_name.empty());
    REQUIRE(own->status.has_value());
    CHECK(own->status->id == *mine);
    CHECK(own->status->params.center == 131'072);
    CHECK(own->status->owned_by_caller);
    CHECK(own->status->owner_kind == rpc::VrxOwnerKind::ThisSession);
    CHECK(own->reason.empty());

    CHECK(foreign->owner == rpc::VrxOwnerKind::OtherSession);
    REQUIRE(foreign->status.has_value());
    CHECK(foreign->status->params.center == -262'144);
    CHECK_FALSE(foreign->status->owned_by_caller);
    CHECK(foreign->status->owner_kind == rpc::VrxOwnerKind::OtherSession);

    // The replay is exactly what exists: two added and nothing else.
    const auto events = log->events();
    CHECK(events.size() == 2);
    CHECK(std::ranges::all_of(
        events, [](const rpc::VrxEvent& e) { return e.kind == rpc::VrxEventKind::Added; }));

    // Relative to the subscriber: the other session sees the same two the
    // other way round.
    auto mirror = std::make_shared<EventLog>();
    subscribe(**other, mirror);
    const auto seen_mine = mirror->wait_for(rpc::VrxEventKind::Added, *mine);
    const auto seen_theirs = mirror->wait_for(rpc::VrxEventKind::Added, *theirs);
    INFO(mirror->describe());
    REQUIRE(seen_mine.has_value());
    REQUIRE(seen_theirs.has_value());
    CHECK(seen_mine->owner == rpc::VrxOwnerKind::OtherSession);
    CHECK(seen_theirs->owner == rpc::VrxOwnerKind::ThisSession);

    (*other)->unsubscribe_vrx_events();
    harness.client().unsubscribe_vrx_events();
    CHECK_FALSE(log->ended());
}

TEST_CASE("this session's receiver is announced as it is added, retuned and removed",
          "[gpu][rpc][vrx-events]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto log = std::make_shared<EventLog>();
    subscribe(harness.client(), log);

    auto vrx = harness.client().add_vrx(receiver_at(131'072));
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    const auto added = log->wait_for(rpc::VrxEventKind::Added, *vrx);
    INFO(log->describe());
    REQUIRE(added.has_value());
    CHECK(added->owner == rpc::VrxOwnerKind::ThisSession);
    REQUIRE(added->status.has_value());
    CHECK(added->status->params.center == 131'072);

    REQUIRE(harness.client().set_vrx_params(*vrx, receiver_at(140'000)).has_value());
    const auto changed = log->wait_for(rpc::VrxEventKind::Changed, *vrx);
    INFO(log->describe());
    REQUIRE(changed.has_value());
    CHECK(changed->owner == rpc::VrxOwnerKind::ThisSession);
    REQUIRE(changed->status.has_value());
    // The tuning AT THE CHANGE, which is what the status in an event is for.
    CHECK(changed->status->params.center == 140'000);

    REQUIRE(harness.client().remove_vrx(*vrx).has_value());
    const auto removed = log->wait_for(rpc::VrxEventKind::Removed, *vrx);
    INFO(log->describe());
    REQUIRE(removed.has_value());
    CHECK(removed->owner == rpc::VrxOwnerKind::ThisSession);
    CHECK_FALSE(removed->status.has_value());
    CHECK(removed->reason == "the receiver was removed");

    // In the order they happened and nothing between them.
    const auto events = about(log->events(), *vrx);
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == rpc::VrxEventKind::Added);
    CHECK(events[1].kind == rpc::VrxEventKind::Changed);
    CHECK(events[2].kind == rpc::VrxEventKind::Removed);
}

TEST_CASE("another session's receiver is announced as theirs and goes when they do",
          "[gpu][rpc][vrx-events]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto log = std::make_shared<EventLog>();
    subscribe(harness.client(), log);

    std::uint64_t vrx = 0;
    {
        auto other = harness.connect_another();
        INFO(test::message_of(other));
        REQUIRE(other.has_value());
        auto made = (*other)->add_vrx(receiver_at(-262'144));
        INFO(test::message_of(made));
        REQUIRE(made.has_value());
        vrx = *made;

        const auto added = log->wait_for(rpc::VrxEventKind::Added, vrx);
        INFO(log->describe());
        REQUIRE(added.has_value());
        CHECK(added->owner == rpc::VrxOwnerKind::OtherSession);
        CHECK(added->owner_name.empty());
        REQUIRE(added->status.has_value());
        CHECK_FALSE(added->status->owned_by_caller);
        CHECK(added->status->creator_session != 0);

        // Destroyed without removing anything, as a crashed window would be.
    }

    const auto removed = log->wait_for(rpc::VrxEventKind::Removed, vrx);
    INFO(log->describe());
    REQUIRE(removed.has_value());
    // Still theirs as it goes: the owner is the record's before it was erased.
    CHECK(removed->owner == rpc::VrxOwnerKind::OtherSession);
    CHECK(removed->reason.find("session that created this receiver ended") !=
          std::string::npos);
}

TEST_CASE("an RDS companion is never announced", "[gpu][rpc][vrx-events]") {
    REVENANT_NEEDS_GPU();

    // A WFM listener below the composite rate has its RDS read from a
    // companion the server opens beside it (ServerImpl::open_rds). Four
    // channels put a coarse channel at 600008 S/s, which carries the 342000
    // the composite is demodulated at; test_rpc_rds.cpp has the arithmetic.
    // No station is needed: the companion exists from the first rdsStation
    // call whether or not anything is there to decode.
    HarnessOptions options;
    options.channels = 4;
    Harness harness;
    bring_up(harness, options);

    auto log = std::make_shared<EventLog>();
    subscribe(harness.client(), log);

    const rpc::VrxParams listening{.center = 5'000,
                                   .bandwidth = 200'000,
                                   .demod = rpc::Demod::Wfm,
                                   .audio_rate = 48'000};
    auto vrx = harness.client().add_vrx(listening);
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto station = harness.client().rds_station(*vrx);
    INFO(test::message_of(station));
    REQUIRE(station.has_value());
    REQUIRE(harness.engine().vrx_ids().size() == 2);

    // A late subscriber's replay leaves the companion out too.
    auto late = std::make_shared<EventLog>();
    {
        auto other = harness.connect_another();
        INFO(test::message_of(other));
        REQUIRE(other.has_value());
        subscribe(**other, late);
        REQUIRE(late->wait_for(rpc::VrxEventKind::Added, *vrx).has_value());

        // Removing the listener takes the companion with it, through the
        // same after_vrx_removed path a front-end retune would take.
        REQUIRE(harness.client().remove_vrx(*vrx).has_value());
        CHECK(harness.engine().vrx_ids().empty());
        REQUIRE(log->wait_for(rpc::VrxEventKind::Removed, *vrx).has_value());

        // The fence: everything before its added has been sent.
        auto fence = harness.client().add_vrx(receiver_at(131'072));
        INFO(test::message_of(fence));
        REQUIRE(fence.has_value());
        REQUIRE(log->wait_for(rpc::VrxEventKind::Added, *fence).has_value());
        REQUIRE(late->wait_for(rpc::VrxEventKind::Added, *fence).has_value());

        for (const auto& seen : {log, late}) {
            INFO(seen->describe());
            for (const rpc::VrxEvent& event : seen->events()) {
                CHECK((event.vrx == *vrx || event.vrx == *fence));
            }
        }
        (*other)->unsubscribe_vrx_events();
    }
}

TEST_CASE("closing the source announces every receiver's removal", "[gpu][rpc][vrx-events]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto one = harness.client().add_vrx(receiver_at(131'072));
    auto two = harness.client().add_vrx(receiver_at(-262'144), rpc::VrxLifetime::Kept);
    INFO(test::message_of(one));
    INFO(test::message_of(two));
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());

    auto log = std::make_shared<EventLog>();
    subscribe(harness.client(), log);
    REQUIRE(log->wait_for(rpc::VrxEventKind::Added, *two).has_value());

    // The engine reports no per-receiver removal for a close; the server has
    // to say it for each one, or a client that never polls keeps them both.
    const Status closed = harness.client().close_source();
    INFO(test::message_of(closed));
    REQUIRE(closed.has_value());

    for (const std::uint64_t vrx : {*one, *two}) {
        const auto removed = log->wait_for(rpc::VrxEventKind::Removed, vrx);
        INFO(log->describe());
        REQUIRE(removed.has_value());
        CHECK(removed->owner == rpc::VrxOwnerKind::ThisSession);
        CHECK(removed->reason.find("source was closed") != std::string::npos);
    }

    // And a receiver on the next source is announced as usual, which is
    // also the fence for "nothing was said twice".
    const Status reopened = harness.client().open_source(test::scene_uri(2'400'032));
    INFO(test::message_of(reopened));
    REQUIRE(reopened.has_value());
    auto fresh = harness.client().add_vrx(receiver_at(131'072));
    INFO(test::message_of(fresh));
    REQUIRE(fresh.has_value());
    REQUIRE(log->wait_for(rpc::VrxEventKind::Added, *fresh).has_value());
    CHECK(about(log->events(), *one).size() == 2);
    CHECK(about(log->events(), *two).size() == 2);
}

TEST_CASE("vrxStatus classifies the owner against the session asking",
          "[gpu][rpc][vrx-events]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto mine = harness.client().add_vrx(receiver_at(131'072));
    INFO(test::message_of(mine));
    REQUIRE(mine.has_value());

    auto other = harness.connect_another();
    INFO(test::message_of(other));
    REQUIRE(other.has_value());

    auto from_me = harness.client().vrx_status(*mine);
    auto from_them = (*other)->vrx_status(*mine);
    INFO(test::message_of(from_me));
    INFO(test::message_of(from_them));
    REQUIRE(from_me.has_value());
    REQUIRE(from_them.has_value());
    CHECK(from_me->owner_kind == rpc::VrxOwnerKind::ThisSession);
    CHECK(from_them->owner_kind == rpc::VrxOwnerKind::OtherSession);
    CHECK(from_me->owner_name.empty());
    CHECK(from_them->owner_name.empty());

    // A receiver no session created, which the hosting process adds straight
    // to the engine. Added from this thread while nothing else drives the
    // engine: no call is in flight, the source is not running, and this
    // server has no transcriber whose reconcile would read the receiver list.
    auto hosted = harness.engine().add_vrx(engine::VrxParams{
        .center = -131'072, .bandwidth = 12'000, .demod = engine::Demod::Nfm});
    INFO(test::message_of(hosted));
    REQUIRE(hosted.has_value());
    auto from_host = harness.client().vrx_status(hosted->value);
    INFO(test::message_of(from_host));
    REQUIRE(from_host.has_value());
    CHECK(from_host->owner_kind == rpc::VrxOwnerKind::Engine);
    CHECK(from_host->creator_session == 0);
    CHECK(from_host->owner_name.empty());

    // And the replay carries it as the engine's.
    auto log = std::make_shared<EventLog>();
    subscribe(harness.client(), log);
    const auto replayed = log->wait_for(rpc::VrxEventKind::Added, hosted->value);
    INFO(log->describe());
    REQUIRE(replayed.has_value());
    CHECK(replayed->owner == rpc::VrxOwnerKind::Engine);
}
