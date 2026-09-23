// plan_receiver_scroll: the wheel over the fine-tuning display, moving the
// receiver.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>

#include "models/receiver_scroll.h"

using revenant::ui::kReceiverScrollSettleMs;
using revenant::ui::kWheelNotchEighths;
using revenant::ui::plan_receiver_scroll;
using revenant::ui::receiver_scroll_step_hz;
using revenant::ui::scroll_tune_eighths;
using revenant::ui::ReceiverScrollRequest;
using revenant::ui::ReceiverScrollState;

namespace {

ReceiverScrollRequest notch(double eighths, double now_ms, double centre = 98'100'000.0)
{
    ReceiverScrollRequest request;
    request.angle_delta_eighths = eighths;
    request.span_hz = 12'000.0;
    request.centre_hz = centre;
    request.low_hz = 96'900'000.0;
    request.high_hz = 99'300'000.0;
    request.now_ms = now_ms;
    return request;
}

}  // namespace

// Rejects a raw fraction of the span, which walks a receiver in steps of 237
// Hz and lands it on numbers nobody tunes to.
TEST_CASE("the step is a round number sized to the display", "[receiverscroll]")
{
    CHECK(receiver_scroll_step_hz(12'000.0) == 200);
    CHECK(receiver_scroll_step_hz(48'000.0) == 500);
    CHECK(receiver_scroll_step_hz(250'000.0) == 5'000);
    CHECK(receiver_scroll_step_hz(3'000.0) == 50);
    CHECK(receiver_scroll_step_hz(10.0) == 1);
    CHECK(receiver_scroll_step_hz(0.0) == 1);
}

// Rejects an interval on the first notch, which reads as the wheel not
// working.
TEST_CASE("the first notch moves the receiver at once", "[receiverscroll]")
{
    const auto plan = plan_receiver_scroll({}, notch(kWheelNotchEighths, 1000.0));
    CHECK(plan.tune);
    CHECK(plan.centre_hz == 98'100'200.0);
    CHECK(plan.wait_ms == 0.0);
}

// The plan takes the angle already resolved by scroll_tune_eighths, where
// negative is down in frequency.
//
// WHAT THIS CASE'S NAME USED TO SAY: "down the wheel is down in frequency". It
// fed the plan a resolved angle and never a wheel, and since 2026-09-23 a
// wheel rolled towards the operator, which is the down the name meant, tunes
// up. The case below drives the raw wheel.
TEST_CASE("a negative resolved angle moves the receiver down", "[receiverscroll]")
{
    const auto plan = plan_receiver_scroll({}, notch(-2 * kWheelNotchEighths, 1000.0));
    CHECK(plan.centre_hz == 98'099'600.0);
}

// Rejects a passband pane that turns the other way from the span. It resolves
// the wheel through the same scroll_tune_eighths the spectrum, the waterfall
// and the ruler do, so a wheel rolled away from the operator moves the
// receiver down, the way it moves the front end down over the span.
TEST_CASE("the passband pane's wheel turns the way the span's does", "[receiverscroll]")
{
    const auto away =
        plan_receiver_scroll({}, notch(scroll_tune_eighths(0.0, kWheelNotchEighths), 1000.0));
    REQUIRE(away.tune);
    CHECK(away.centre_hz == 98'099'800.0);

    const auto towards =
        plan_receiver_scroll({}, notch(scroll_tune_eighths(0.0, -kWheelNotchEighths), 1000.0));
    REQUIRE(towards.tune);
    CHECK(towards.centre_hz == 98'100'200.0);
}

// Rejects sending every event. A flick delivers a dozen inside the interval,
// and they collapse into one move when it has passed.
TEST_CASE("a burst inside the interval is held and sent as one", "[receiverscroll]")
{
    auto plan = plan_receiver_scroll({}, notch(kWheelNotchEighths, 1000.0));
    REQUIRE(plan.tune);
    auto state = plan.state;

    for (int i = 1; i <= 4; ++i) {
        plan = plan_receiver_scroll(state, notch(kWheelNotchEighths, 1000.0 + i * 5.0,
                                                 98'100'200.0));
        CHECK_FALSE(plan.tune);
        CHECK(plan.wait_ms > 0.0);
        state = plan.state;
    }

    plan = plan_receiver_scroll(state, notch(0.0, 1000.0 + kReceiverScrollSettleMs,
                                             98'100'200.0));
    CHECK(plan.tune);
    CHECK(plan.centre_hz == 98'100'200.0 + 4 * 200.0);
}

// Rejects rounding a touchpad's fractions away, or up. They bank until they
// make a whole notch.
TEST_CASE("fractions of a notch add up rather than vanish", "[receiverscroll]")
{
    ReceiverScrollState state;
    auto plan = plan_receiver_scroll(state, notch(40.0, 1000.0));
    CHECK_FALSE(plan.tune);
    state = plan.state;
    plan = plan_receiver_scroll(state, notch(40.0, 1001.0));
    CHECK_FALSE(plan.tune);
    state = plan.state;
    plan = plan_receiver_scroll(state, notch(40.0, 1002.0));
    CHECK(plan.tune);
    CHECK(plan.centre_hz == 98'100'200.0);
}

// A receiver outside the span is one the engine removes, so the wheel stops
// at the edge and does not bank travel against it.
TEST_CASE("the span's edge stops the receiver", "[receiverscroll]")
{
    auto plan = plan_receiver_scroll({}, notch(3 * kWheelNotchEighths, 1000.0, 99'299'900.0));
    CHECK(plan.tune);
    CHECK(plan.centre_hz == 99'300'000.0);

    plan = plan_receiver_scroll(plan.state, notch(kWheelNotchEighths, 2000.0, 99'300'000.0));
    CHECK_FALSE(plan.tune);
    CHECK(plan.state.pending_eighths == 0.0);
}

TEST_CASE("no frame yet means nothing to scale a notch against", "[receiverscroll]")
{
    auto request = notch(kWheelNotchEighths, 1000.0);
    request.span_hz = 0.0;
    const auto plan = plan_receiver_scroll({}, request);
    CHECK_FALSE(plan.tune);
    CHECK(plan.state.pending_eighths == 0.0);
}
