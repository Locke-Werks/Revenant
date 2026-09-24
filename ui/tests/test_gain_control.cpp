// The gain slider's arithmetic.
//
// The cases that matter are the ones about NOT LYING: a handle placed from the
// request rather than from the device's answer shows a gain the tuner is not
// on, and on a stepped stage that is most positions of the slider.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "models/gain_control.h"

using Catch::Approx;
using revenant::ui::fraction_for_gain;
using revenant::ui::gain_for_fraction;
using revenant::ui::gain_fraction_step;
using revenant::ui::gain_request_for_fraction;
using revenant::ui::gain_stage_usable;

namespace {

// The R820T's own table, which is the stage this control was written against:
// 29 steps from 0 to 49.6 dB, unevenly spaced. The gaps run from 0.9 dB to
// 4.2 dB, which is why the step size comes off the count and not off a gap.
revenant::rpc::GainStage r820t()
{
    revenant::rpc::GainStage stage;
    stage.name = "tuner";
    stage.min_db = 0.0;
    stage.max_db = 49.6;
    stage.has_auto = true;
    stage.steps_db = {0.0,  0.9,  1.4,  2.7,  3.7,  7.7,  8.7,  12.5, 14.4, 15.7,
                      16.6, 19.7, 20.7, 22.9, 25.4, 28.0, 29.7, 32.8, 33.8, 36.4,
                      37.2, 38.6, 40.2, 42.1, 43.4, 43.9, 44.5, 48.0, 49.6};
    return stage;
}

// A stage with travel and no steps, which is what a device reporting a
// continuous control looks like.
revenant::rpc::GainStage continuous()
{
    revenant::rpc::GainStage stage;
    stage.name = "if";
    stage.min_db = -10.0;
    stage.max_db = 30.0;
    return stage;
}

}  // namespace

TEST_CASE("a stage with no travel gets no slider", "[gain]")
{
    // A file and a synthetic scene report no stages at all, so this is the
    // other case: a device naming a control whose ends are the same. A slider
    // over it would move and do nothing, which is worse than no slider.
    revenant::rpc::GainStage flat;
    flat.name = "tuner";
    flat.min_db = 20.0;
    flat.max_db = 20.0;
    CHECK_FALSE(gain_stage_usable(flat));

    revenant::rpc::GainStage crossed;
    crossed.min_db = 30.0;
    crossed.max_db = 10.0;
    CHECK_FALSE(gain_stage_usable(crossed));

    CHECK(gain_stage_usable(r820t()));
    CHECK(gain_stage_usable(continuous()));

    // And the accessors answer the bottom of the range rather than propagating
    // nonsense, because a NaN or a wild number reaching the wire is a refusal
    // that tells the operator nothing about the slider that produced it.
    CHECK(gain_for_fraction(flat, 0.5) == Approx(20.0));
    CHECK(fraction_for_gain(flat, 20.0) == Approx(0.0));
    CHECK(gain_fraction_step(flat) == Approx(0.0));
}

TEST_CASE("the slider maps across the stage's decibel range", "[gain]")
{
    const revenant::rpc::GainStage stage = r820t();

    CHECK(gain_for_fraction(stage, 0.0) == Approx(0.0));
    CHECK(gain_for_fraction(stage, 1.0) == Approx(49.6));
    CHECK(gain_for_fraction(stage, 0.5) == Approx(24.8));

    // A drag that overshoots its own ends is clamped, not refused. QML hands
    // over a hair past 0 and 1 from a fast drag, and refusing there would make
    // the two ends of the travel unreachable.
    CHECK(gain_for_fraction(stage, -0.2) == Approx(0.0));
    CHECK(gain_for_fraction(stage, 1.4) == Approx(49.6));

    // Not finite is the minimum, which is the quiet direction. See settle_gain
    // in models/source_choice.h for why less gain is the safe way to be wrong.
    CHECK(gain_for_fraction(stage, std::nan("")) == Approx(0.0));
}

TEST_CASE("the handle is placed from the gain the device took", "[gain]")
{
    const revenant::rpc::GainStage stage = r820t();

    // THE CASE THIS CONTROL EXISTS FOR. Ask for 24.8 dB, the middle of the
    // travel, and the tuner has no such step: the nearest are 22.9 and 25.4,
    // and 25.4 is 0.6 away against 1.9. The handle has to go where 25.4 is,
    // not where the pointer was left, or the slider is showing a gain the tuner
    // is not on.
    const double asked = gain_for_fraction(stage, 0.5);
    const double granted = gain_request_for_fraction(stage, 0.5);
    CHECK(asked == Approx(24.8));
    CHECK(granted == Approx(25.4));
    CHECK(fraction_for_gain(stage, granted) == Approx(25.4 / 49.6));

    // The round trip is exact at a step, which is what stops the handle
    // drifting when the same value is set twice.
    CHECK(gain_request_for_fraction(stage, fraction_for_gain(stage, 22.9)) == Approx(22.9));
    CHECK(gain_request_for_fraction(stage, fraction_for_gain(stage, 0.0)) == Approx(0.0));
    CHECK(gain_request_for_fraction(stage, fraction_for_gain(stage, 49.6)) == Approx(49.6));

    // A REQUEST EXACTLY BETWEEN TWO STEPS TAKES THE LOWER ONE, and the numbers
    // here are chosen so the tie is exact rather than nearly exact: a fraction
    // of 0.25 over a 0 to 20 dB stage asks for 5.0, which is the same distance
    // from 0 and from 10. settle_gain's rule is that the safe direction is
    // less, because too much gain drives the front end past its linear range
    // and puts intermodulation products in the detector's list at full
    // confidence. README.md has that measured on air.
    revenant::rpc::GainStage tie;
    tie.name = "tuner";
    tie.min_db = 0.0;
    tie.max_db = 20.0;
    tie.steps_db = {0.0, 10.0, 20.0};
    CHECK(gain_for_fraction(tie, 0.25) == Approx(5.0));
    CHECK(gain_request_for_fraction(tie, 0.25) == Approx(0.0));

    // A continuous stage grants what was asked, so the handle stays put.
    const revenant::rpc::GainStage smooth = continuous();
    CHECK(gain_request_for_fraction(smooth, 0.25) == Approx(0.0));
    CHECK(fraction_for_gain(smooth, 0.0) == Approx(0.25));
}

TEST_CASE("a key press moves one step on average and nothing on a continuous stage", "[gain]")
{
    const revenant::rpc::GainStage stage = r820t();

    // 29 steps, so 28 gaps. One press crosses one gap on average, which is
    // what the count buys: the gaps are 0.9 dB at the bottom and 4.2 dB in the
    // middle, so any single gap taken as the step size would skip stages at one
    // end and take several presses each at the other.
    CHECK(gain_fraction_step(stage) == Approx(1.0 / 28.0));

    // Zero for a continuous stage, which QML reads as no stepping. An invented
    // step size there would make part of the range unreachable from the
    // keyboard.
    CHECK(gain_fraction_step(continuous()) == Approx(0.0));

    // And zero for a stage reporting a single step, which is a device with one
    // gain rather than a control.
    revenant::rpc::GainStage only_one = r820t();
    only_one.steps_db = {20.0};
    CHECK(gain_fraction_step(only_one) == Approx(0.0));
}

// Rejects ticks drawn at an even spacing, which is what the handle's key step
// is and what the tuner is not. The R820T's steps bunch at the bottom and the
// top of its range, and a tick belongs where a step is.
TEST_CASE("the control row's ticks sit on the tuner's own steps", "[gain]")
{
    const revenant::rpc::GainStage stage = r820t();
    const std::vector<double> ticks = revenant::ui::gain_step_fractions(stage);
    REQUIRE(ticks.size() == stage.steps_db.size());
    CHECK(ticks.front() == Approx(0.0));
    CHECK(ticks.back() == Approx(1.0));
    CHECK(ticks[1] == Approx(0.9 / 49.6));
    CHECK(ticks[5] == Approx(7.7 / 49.6));

    // Every tick is a position the slider settles onto the same step from.
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        CHECK(gain_request_for_fraction(stage, ticks[i]) == Approx(stage.steps_db[i]));
    }

    CHECK(revenant::ui::gain_step_fractions(continuous()).empty());
}

// Rejects a disabled slider with nothing to say why, which reads as a control
// that is broken. A recording, a synthetic scene and no source at all are
// three different reasons, and the owner opens recordings as often as radios.
TEST_CASE("a source with no gain says why", "[gain]")
{
    using revenant::ui::gain_absence;
    CHECK(gain_absence(true, true, "rtlsdr").word.empty());
    CHECK(gain_absence(true, false, "file").word == "recording");
    CHECK(gain_absence(true, false, "synthetic").word == "synthetic");
    CHECK(gain_absence(true, false, "somethingelse").word == "no gain");
    CHECK(gain_absence(false, false, "").word == "no radio");
    CHECK_FALSE(gain_absence(true, false, "file").sentence.empty());
}
