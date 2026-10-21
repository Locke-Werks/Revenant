// The trunking follower's state machine, driven with made-up grants and
// times: which grants get a receiver, when receivers move or go, and what
// the call log says about each.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "core/engine/trunk_follower.h"

using namespace revenant::engine;

namespace {

TrunkGrant grant(std::uint32_t tg, std::int64_t hz, bool initial = true, bool encrypted = false) {
    TrunkGrant g;
    g.talkgroup = tg;
    g.frequency_hz = hz;
    g.initial = initial;
    g.options_known = initial;
    g.encrypted = encrypted;
    g.source = initial ? std::optional<std::uint32_t>(9000 + tg) : std::nullopt;
    return g;
}

// Carries out every Open with the next receiver id, as the server would.
std::vector<TrunkAction> settle(TrunkFollower& f, std::uint64_t& next_vrx) {
    auto actions = f.take_actions();
    for (const TrunkAction& a : actions) {
        if (a.kind == TrunkAction::Kind::Open) {
            f.opened(a.call, next_vrx++);
        }
    }
    return actions;
}

const TrunkCall* active_tg(const TrunkFollower& f, std::uint32_t tg) {
    for (const TrunkCall& c : f.active()) {
        if (c.talkgroup == tg) {
            return &c;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("trunk follower: a grant opens a receiver and a terminator releases it",
          "[trunk_follower]") {
    TrunkFollower f;
    std::uint64_t vrx = 10;
    f.grant(grant(100, 852'000'000), 0.0);
    auto actions = settle(f, vrx);
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == TrunkAction::Kind::Open);
    CHECK(actions[0].frequency_hz == 852'000'000);
    REQUIRE(active_tg(f, 100) != nullptr);
    CHECK(active_tg(f, 100)->vrx == 10);
    CHECK(active_tg(f, 100)->source == 9100U);

    f.voice(10, TrunkVoice{false, 100, 4242, false}, 0.5);
    CHECK(active_tg(f, 100)->source == 4242U);
    f.voice(10, TrunkVoice{true, {}, {}, false}, 1.0);

    // Updates during the hold do not keep a terminated call open.
    f.grant(grant(100, 852'000'000, false), 1.5);
    f.tick(2.9);
    CHECK(f.take_actions().empty());
    f.tick(3.0);
    actions = f.take_actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == TrunkAction::Kind::Release);
    CHECK(actions[0].vrx == 10);

    const auto log = f.calls();
    REQUIRE(log.size() == 1);
    CHECK_FALSE(log[0].active);
    CHECK(log[0].followed);
    CHECK(log[0].end_reason == "call ended");
    CHECK(log[0].start_s == 0.0);
    CHECK(log[0].end_s == 1.0);
}

TEST_CASE("trunk follower: an initial grant during the hold keeps the receiver",
          "[trunk_follower]") {
    TrunkFollower f;
    std::uint64_t vrx = 1;
    f.grant(grant(100, 852'000'000), 0.0);
    settle(f, vrx);
    f.voice(1, TrunkVoice{true, {}, {}, false}, 1.0);
    f.grant(grant(100, 852'000'000, true), 1.5);
    f.tick(3.2);
    CHECK(f.take_actions().empty());
    REQUIRE(active_tg(f, 100) != nullptr);
}

TEST_CASE("trunk follower: hang time ends a call nobody hears", "[trunk_follower]") {
    TrunkFollower f;
    std::uint64_t vrx = 1;
    f.grant(grant(100, 852'000'000), 0.0);
    settle(f, vrx);
    f.tick(2.4);
    CHECK(f.take_actions().empty());
    f.tick(2.5);
    const auto actions = f.take_actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == TrunkAction::Kind::Release);
    CHECK(f.calls().front().end_reason == "nothing heard");
}

TEST_CASE("trunk follower: encrypted calls are logged and never followed", "[trunk_follower]") {
    TrunkFollower f;
    std::uint64_t vrx = 1;
    f.grant(grant(300, 853'000'000, true, true), 0.0);
    CHECK(settle(f, vrx).empty());
    REQUIRE(active_tg(f, 300) != nullptr);
    CHECK(active_tg(f, 300)->encrypted);
    CHECK(active_tg(f, 300)->skip == TrunkSkip::Encrypted);

    // An update with no options for the same talkgroup is still encrypted.
    f.grant(grant(300, 853'012'500, false), 0.5);
    CHECK(f.take_actions().empty());

    // Clear by its grant and encrypted by its voice: the receiver goes.
    f.grant(grant(400, 853'100'000), 1.0);
    settle(f, vrx);
    const std::uint64_t followed = active_tg(f, 400)->vrx;
    f.voice(followed, TrunkVoice{false, 400, {}, true}, 1.2);
    const auto actions = f.take_actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == TrunkAction::Kind::Release);
    CHECK(active_tg(f, 400)->encrypted);
    CHECK(active_tg(f, 400)->vrx == 0);
}

TEST_CASE("trunk follower: allow and deny lists", "[trunk_follower]") {
    TrunkFollower f;
    std::uint64_t vrx = 1;
    TrunkRules rules;
    rules.allow = {100, 200};
    rules.deny = {200};
    f.set_rules(rules, 0.0);

    f.grant(grant(100, 852'000'000), 0.0);
    f.grant(grant(200, 852'100'000), 0.0);
    f.grant(grant(300, 852'200'000), 0.0);
    const auto actions = settle(f, vrx);
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].frequency_hz == 852'000'000);
    CHECK(active_tg(f, 200)->skip == TrunkSkip::Denied);
    CHECK(active_tg(f, 300)->skip == TrunkSkip::NotAllowed);

    // Lifting the deny follows the call already on the air.
    rules.deny.clear();
    f.set_rules(rules, 0.5);
    const auto later = settle(f, vrx);
    REQUIRE(later.size() == 1);
    CHECK(later[0].frequency_hz == 852'100'000);
}

TEST_CASE("trunk follower: priority takes a busy receiver by retuning it",
          "[trunk_follower]") {
    TrunkSettings settings;
    settings.max_followers = 1;
    TrunkFollower f(settings);
    TrunkRules rules;
    rules.priority = {{500, 5}};
    f.set_rules(rules, 0.0);
    std::uint64_t vrx = 7;

    f.grant(grant(100, 852'000'000), 0.0);
    settle(f, vrx);
    f.grant(grant(101, 852'050'000), 0.1);
    CHECK(f.take_actions().empty());
    CHECK(active_tg(f, 101)->skip == TrunkSkip::Busy);

    f.grant(grant(500, 852'100'000), 0.2);
    auto actions = f.take_actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == TrunkAction::Kind::Retune);
    CHECK(actions[0].vrx == 7);
    CHECK(actions[0].frequency_hz == 852'100'000);
    CHECK(active_tg(f, 100)->vrx == 0);
    CHECK(active_tg(f, 500)->vrx == 7);

    // When the priority call ends its receiver goes to the oldest waiting
    // call still on the air rather than being removed.
    f.grant(grant(100, 852'000'000, false), 1.0);
    f.grant(grant(101, 852'050'000, false), 1.0);
    f.voice(7, TrunkVoice{true, {}, {}, false}, 1.0);
    f.tick(3.0);
    actions = f.take_actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == TrunkAction::Kind::Retune);
    CHECK(actions[0].frequency_hz == 852'000'000);
}

TEST_CASE("trunk follower: span limits and refusals", "[trunk_follower]") {
    TrunkFollower f;
    std::uint64_t vrx = 1;
    f.set_span(851'000'000, 854'000'000, 0.0);
    f.grant(grant(100, 860'000'000), 0.0);
    CHECK(f.take_actions().empty());
    CHECK(active_tg(f, 100)->skip == TrunkSkip::OutsideSpan);

    f.grant(grant(200, 852'000'000), 0.0);
    auto actions = f.take_actions();
    REQUIRE(actions.size() == 1);
    f.open_failed(actions[0].call, 0.1);
    CHECK(active_tg(f, 200)->skip == TrunkSkip::Refused);
    f.grant(grant(200, 852'000'000, false), 1.0);
    CHECK(f.take_actions().empty());
    f.grant(grant(200, 852'000'000, false), 5.2);
    actions = settle(f, vrx);
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == TrunkAction::Kind::Open);

    // A retune that leaves the call outside drops its receiver.
    f.set_span(855'000'000, 858'000'000, 5.5);
    actions = f.take_actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == TrunkAction::Kind::Release);
}

TEST_CASE("trunk follower: a reassigned channel keeps its receiver", "[trunk_follower]") {
    TrunkFollower f;
    std::uint64_t vrx = 1;
    f.grant(grant(100, 852'000'000), 0.0);
    settle(f, vrx);
    f.grant(grant(200, 852'000'000), 1.0);
    CHECK(f.take_actions().empty());
    REQUIRE(active_tg(f, 200) != nullptr);
    CHECK(active_tg(f, 200)->vrx == 1);
    CHECK(f.calls().back().end_reason == "channel reassigned");

    // The voice naming a denied talkgroup takes it away.
    TrunkRules rules;
    rules.deny = {300};
    f.set_rules(rules, 1.1);
    f.voice(1, TrunkVoice{false, 300, {}, false}, 1.2);
    const auto actions = f.take_actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == TrunkAction::Kind::Release);
    CHECK(active_tg(f, 300)->skip == TrunkSkip::Denied);
}

TEST_CASE("trunk follower: an open landing after its call ended is released",
          "[trunk_follower]") {
    TrunkFollower f;
    f.grant(grant(100, 852'000'000), 0.0);
    const auto actions = f.take_actions();
    REQUIRE(actions.size() == 1);
    f.tick(3.0);
    CHECK(f.take_actions().empty());
    f.opened(actions[0].call, 44);
    const auto release = f.take_actions();
    REQUIRE(release.size() == 1);
    CHECK(release[0].kind == TrunkAction::Kind::Release);
    CHECK(release[0].vrx == 44);
}

TEST_CASE("trunk follower: stop and an outside removal", "[trunk_follower]") {
    TrunkFollower f;
    std::uint64_t vrx = 1;
    f.grant(grant(100, 852'000'000), 0.0);
    f.grant(grant(200, 852'100'000), 0.0);
    settle(f, vrx);
    f.vrx_gone(1, 0.5);
    CHECK(f.take_actions().empty());
    CHECK(active_tg(f, 100) == nullptr);

    f.stop("control channel lost", 0.6);
    const auto actions = f.take_actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].vrx == 2);
    CHECK(f.active().empty());
    CHECK(f.calls().front().end_reason == "control channel lost");
}
