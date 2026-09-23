// The rack's rules in models/receiver_rack.h: slots, focus, solo and mute,
// the strip gain, and what a click on the span does.
//
// Each case names the wrong implementation it rejects, on the convention
// test_receiver_marker.cpp states.

#include <cstdint>
#include <set>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "models/receiver_rack.h"

using Catch::Approx;
using revenant::ui::classify_span_click;
using revenant::ui::kMaxReceivers;
using revenant::ui::rack_gain_amplitude;
using revenant::ui::rack_gain_text;
using revenant::ui::RackBand;
using revenant::ui::receiver_under;
using revenant::ui::ReceiverRack;
using revenant::ui::SpanClick;
using revenant::ui::SpanClickInput;

// Rejects a rack that lets a ninth receiver share a colour, or that hands the
// same slot to two receivers. The slot is the colour and the audio ring, so a
// shared one is two receivers drawn alike and mixed from one buffer.
TEST_CASE("the rack holds eight, each on a slot of its own")
{
    ReceiverRack rack;
    std::set<std::size_t> slots;
    for (std::size_t i = 0; i < kMaxReceivers; ++i) {
        const auto key = rack.add();
        REQUIRE(key.has_value());
        slots.insert(rack.find(*key)->slot);
    }
    CHECK(slots.size() == kMaxReceivers);
    CHECK(rack.full());
    CHECK_FALSE(rack.add().has_value());
}

// Rejects slots handed out by count rather than by what is free. Removing
// the second of three and adding one must reuse slot 1, or the fourth
// receiver is drawn in the third one's colour.
TEST_CASE("a removed receiver's colour is the next one handed out")
{
    ReceiverRack rack;
    const auto a = *rack.add();
    const auto b = *rack.add();
    const auto c = *rack.add();
    CHECK(rack.find(a)->slot == 0);
    CHECK(rack.find(b)->slot == 1);
    CHECK(rack.find(c)->slot == 2);
    rack.remove(b);
    const auto d = *rack.add();
    CHECK(rack.find(d)->slot == 1);
    CHECK(rack.find(c)->slot == 2);

    // And keys are never reused, so a stale key cannot name the new one.
    CHECK(d != b);
    CHECK(rack.find(b) == nullptr);
}

// Rejects focus left on a receiver that has gone, which is a detail pane
// claiming a receiver the rack no longer shows.
TEST_CASE("removing the focused receiver moves focus the way closing a tab does")
{
    ReceiverRack rack;
    const auto a = *rack.add();
    const auto b = *rack.add();
    const auto c = *rack.add();
    REQUIRE(rack.focus(b));
    CHECK(rack.remove(b) == c);
    CHECK(rack.focused() == c);
    CHECK(rack.remove(c) == a);
    CHECK(rack.remove(a) == 0);
    CHECK(rack.focused() == 0);

    // Removing one that is not focused leaves focus alone.
    const auto d = *rack.add();
    const auto e = *rack.add();
    REQUIRE(rack.focus(d));
    CHECK(rack.remove(e) == d);
}

// Rejects next and previous that stop at the ends or step from the wrong
// place.
TEST_CASE("next and previous wrap around the rack")
{
    ReceiverRack rack;
    CHECK(rack.neighbour(1) == 0);
    const auto a = *rack.add();
    CHECK(rack.neighbour(1) == 0);
    const auto b = *rack.add();
    const auto c = *rack.add();
    REQUIRE(rack.focus(a));
    CHECK(rack.neighbour(1) == b);
    CHECK(rack.neighbour(-1) == c);
    REQUIRE(rack.focus(c));
    CHECK(rack.neighbour(1) == a);
    CHECK_FALSE(rack.focus(999));
    CHECK(rack.focused() == c);
}

// Rejects a solo that adds to other solos, and a mute that outranks a solo.
TEST_CASE("solo is exclusive and outranks mute")
{
    ReceiverRack rack;
    const auto a = *rack.add();
    const auto b = *rack.add();
    const auto c = *rack.add();
    rack.find(b)->muted = true;

    CHECK(rack.heard(a));
    CHECK_FALSE(rack.heard(b));
    CHECK(rack.heard(c));

    CHECK(rack.toggle_solo(a));
    CHECK(rack.heard(a));
    CHECK_FALSE(rack.heard(b));
    CHECK_FALSE(rack.heard(c));

    // A second solo replaces the first.
    CHECK(rack.toggle_solo(b));
    CHECK_FALSE(rack.find(a)->solo);
    CHECK(rack.heard(b));
    CHECK_FALSE(rack.heard(a));

    // Off again, and the mutes are what they were.
    CHECK_FALSE(rack.toggle_solo(b));
    CHECK_FALSE(rack.any_solo());
    CHECK(rack.heard(a));
    CHECK_FALSE(rack.heard(b));
    CHECK(rack.heard(c));
}

// Rejects a gain control linear in amplitude, which does its audible work
// in the top fifth of its travel, and one whose bottom is -40 dB rather than
// off.
TEST_CASE("the strip gain is decibels over its travel and off at the bottom")
{
    CHECK(rack_gain_amplitude(1.0) == Approx(1.0));
    CHECK(rack_gain_amplitude(0.5) == Approx(0.1));
    CHECK(rack_gain_amplitude(0.0) == 0.0);
    CHECK(rack_gain_amplitude(-1.0) == 0.0);
    CHECK(rack_gain_amplitude(2.0) == Approx(1.0));
    CHECK(rack_gain_text(1.0) == "0 dB");
    CHECK(rack_gain_text(0.7) == "-12 dB");
    CHECK(rack_gain_text(0.0) == "off");
}

// Rejects a hit test that can target the focused receiver, which would turn
// every fine click inside its own band into a focus that does nothing, and
// one that picks the wide band over the narrow one inside it.
TEST_CASE("a click lands on the narrowest other receiver under it")
{
    const std::vector<RackBand> bands{
        {1, 98'000'000.0, 98'200'000.0},  // a broadcast receiver
        {2, 98'100'000.0, 98'103'000.0},  // a narrow one inside it
        {3, 98'150'000.0, 98'160'000.0},
    };
    CHECK(receiver_under(bands, 98'101'000.0, 3) == 2);
    CHECK(receiver_under(bands, 98'050'000.0, 3) == 1);
    CHECK(receiver_under(bands, 98'155'000.0, 3) == 1);
    CHECK(receiver_under(bands, 98'155'000.0, 1) == 3);
    CHECK(receiver_under(bands, 99'000'000.0, 1) == 0);

    // A band not yet granted has no width and is no target.
    CHECK(receiver_under({{4, 98'000'000.0, 98'000'000.0}}, 98'000'000.0, 0) == 0);
}

// THE CLICK RULE. Rejects a double click that adds a receiver on top of the
// one its own first click just opened, one that adds past eight, and one that
// adds when its first click was a focus.
TEST_CASE("which click does what")
{
    SpanClickInput in;
    in.count = 2;
    in.had_receiver = true;
    CHECK(classify_span_click(in) == SpanClick::Retune);

    in.under = 7;
    CHECK(classify_span_click(in) == SpanClick::Focus);

    in = {};
    CHECK(classify_span_click(in) == SpanClick::Open);

    // Double clicks.
    in = {};
    in.double_click = true;
    in.had_receiver = true;
    in.count = 3;
    CHECK(classify_span_click(in) == SpanClick::Add);

    in.count = kMaxReceivers;
    CHECK(classify_span_click(in) == SpanClick::Full);

    in.count = 3;
    in.under = 5;
    CHECK(classify_span_click(in) == SpanClick::Nothing);

    in.under = 0;
    in.first_click_opened = true;
    CHECK(classify_span_click(in) == SpanClick::Nothing);
}
