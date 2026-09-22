// plan_scroll_tune, which decides whether the mouse wheel over a wide display
// retunes the front end now, later, or not at all.
//
// WHY THIS IS THE PART WORTH ASSERTING. A device retune costs about 330 ms of
// dead stream, measured on an R820T and recorded in docs/rpc.md. What that
// makes expensive is not a wrong frequency, which an operator sees and
// corrects, but a gesture that issues more retunes than the radio can take:
// the picture then keeps moving after the operator stopped scrolling, and it
// is the accumulator and the clock that decide whether it does. Neither is
// visible on screen.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives: a test that only asserts
// what the code already does certifies one reachable shape and reads as though
// it certified the behaviour.
//
// WHAT IS NOT HERE. Whether the source will retune at all, which is
// EngineLink::sourceCanRetune and is gated in take_scroll_tune, and the timer
// that turns wait_ms into a later call, which is a QTimer in a QQuickItem.
// Both need Qt and this target links none.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "models/scroll_tune.h"

using Catch::Approx;
using revenant::ui::kScrollTuneSettleMs;
using revenant::ui::kScrollTuneSpanFraction;
using revenant::ui::kWheelNotchEighths;
using revenant::ui::plan_scroll_tune;
using revenant::ui::scroll_tune_eighths;
using revenant::ui::ScrollTunePlan;
using revenant::ui::ScrollTuneRequest;
using revenant::ui::ScrollTuneState;

namespace {

// The shipped geometry: an RTL-SDR at 2.4 MS/s parked on broadcast FM, with
// the R820T's own tuning range around it.
constexpr double kSpan = 2'400'000.0;
constexpr double kCentre = 98'100'000.0;
constexpr double kTuneLow = 24'000'000.0;
constexpr double kTuneHigh = 1'766'000'000.0;

// One notch of the span, which is what a single detent should move the radio.
constexpr double kStep = kSpan * kScrollTuneSpanFraction;

[[nodiscard]] ScrollTuneRequest wheel(double eighths, double now_ms, double center_hz = kCentre)
{
    ScrollTuneRequest request;
    request.angle_delta_eighths = eighths;
    request.span_hz = kSpan;
    request.center_hz = center_hz;
    request.tune_low_hz = kTuneLow;
    request.tune_high_hz = kTuneHigh;
    request.now_ms = now_ms;
    return request;
}

}  // namespace

// Rejects an implementation that adds the two axes together. A touchpad
// delivers a diagonal as both at once, so a gesture that is mostly vertical
// would tune further than the picture says it should, and by an amount that
// depends on how straight the operator's finger was.
TEST_CASE("the wheel resolves to one number, vertical first")
{
    CHECK(scroll_tune_eighths(0.0, 120.0) == Approx(120.0));
    CHECK(scroll_tune_eighths(120.0, 0.0) == Approx(120.0));

    // Both axes present. The vertical one is the answer and the horizontal one
    // is discarded rather than added to it.
    CHECK(scroll_tune_eighths(40.0, 120.0) == Approx(120.0));

    // Sign survives, because scrolling back is how an overshoot is corrected.
    CHECK(scroll_tune_eighths(0.0, -120.0) == Approx(-120.0));
    CHECK(scroll_tune_eighths(0.0, 0.0) == Approx(0.0));
}

// Rejects an interval imposed on the first notch as well as on the ones after
// it. A coalescer that simply refuses to act until settle_ms has passed since
// it was constructed puts 400 ms of lag on every isolated scroll, which reads
// as the wheel not working rather than as pacing.
TEST_CASE("the first notch tunes immediately")
{
    const ScrollTunePlan plan = plan_scroll_tune(ScrollTuneState{}, wheel(120.0, 0.0));

    REQUIRE(plan.tune);
    CHECK(plan.center_hz == Approx(kCentre + kStep));
    CHECK(plan.wait_ms == Approx(0.0));

    // Spent, so a flush arriving behind it has nothing to do.
    CHECK(plan.state.pending_eighths == Approx(0.0));
    CHECK(plan.state.issued);
}

// Rejects a step fixed in hertz. The same gesture has to feel the same at
// 2.4 MS/s and at 20 MS/s, and a count that suits one of those throws the other
// most of the way across the display or barely moves it.
TEST_CASE("the step is a fraction of the span and not a hertz count")
{
    ScrollTuneRequest narrow = wheel(120.0, 0.0);
    narrow.span_hz = 2'400'000.0;
    const ScrollTunePlan narrow_plan = plan_scroll_tune(ScrollTuneState{}, narrow);

    ScrollTuneRequest wide = wheel(120.0, 0.0);
    wide.span_hz = 20'000'000.0;
    const ScrollTunePlan wide_plan = plan_scroll_tune(ScrollTuneState{}, wide);

    REQUIRE(narrow_plan.tune);
    REQUIRE(wide_plan.tune);

    // A twentieth of each, so the picture moves by the same proportion of its
    // own width in both cases.
    CHECK(narrow_plan.center_hz - kCentre == Approx(2'400'000.0 / 20.0));
    CHECK(wide_plan.center_hz - kCentre == Approx(20'000'000.0 / 20.0));

    // And the step is well inside the span, which is the property that stops a
    // sweep passing over a signal without ever drawing it.
    CHECK(narrow_plan.center_hz - kCentre < narrow.span_hz);
    CHECK(wide_plan.center_hz - kCentre < wide.span_hz);
}

// Rejects rounding each event to a whole notch. A high-resolution wheel and a
// touchpad send fractions of a detent far more often than 120 at a time: a
// floor makes them do nothing at all and a ceiling makes them fly, and both
// look like a broken device rather than a rounding decision.
TEST_CASE("a fractional notch accumulates instead of rounding")
{
    ScrollTuneState state;

    // A touchpad's worth: an eighth of a notch, which lands on a plan that has
    // never issued so it goes out immediately.
    ScrollTunePlan plan = plan_scroll_tune(state, wheel(15.0, 0.0));
    REQUIRE(plan.tune);
    CHECK(plan.center_hz - kCentre == Approx(kStep / 8.0));
    state = plan.state;

    // Seven more of them inside the interval. None tunes, and each one is
    // still in the accumulator rather than discarded.
    double now = 0.0;
    for (int event = 0; event < 7; ++event) {
        now += 20.0;
        plan = plan_scroll_tune(state, wheel(15.0, now));
        CHECK_FALSE(plan.tune);
        CHECK(plan.wait_ms > 0.0);
        state = plan.state;
    }
    CHECK(state.pending_eighths == Approx(105.0));

    // The interval comes due with no wheel behind it, which is the flush the
    // caller's timer makes, and the seven fractions add up to seven eighths of
    // a notch. A rounding implementation would have moved nothing or moved
    // seven whole notches.
    plan = plan_scroll_tune(state, wheel(0.0, kScrollTuneSettleMs + 1.0));
    REQUIRE(plan.tune);
    CHECK(plan.center_hz - kCentre == Approx(kStep * 7.0 / 8.0));
}

// Rejects a pass-through. This is the whole reason the header exists: a burst
// of wheel events sent straight to the source queues one 330 ms stall per
// event, and the radio is still working through them long after the operator
// stopped.
TEST_CASE("a burst inside one interval issues one tune carrying all of it")
{
    ScrollTuneState state;

    // The first goes out, as it should.
    ScrollTunePlan plan = plan_scroll_tune(state, wheel(120.0, 0.0));
    REQUIRE(plan.tune);
    state = plan.state;

    // Nine more at 30 ms apart, which is an ordinary flick and well inside the
    // interval. Not one of them tunes.
    double now = 0.0;
    for (int event = 0; event < 9; ++event) {
        now += 30.0;
        plan = plan_scroll_tune(state, wheel(120.0, now));
        CHECK_FALSE(plan.tune);
        state = plan.state;
    }

    // The interval elapses and the nine go out as one step, from the centre
    // the caller handed over. Nine separate retunes would have cost three
    // seconds of dead stream to reach the same frequency.
    plan = plan_scroll_tune(state, wheel(0.0, kScrollTuneSettleMs + 1.0));
    REQUIRE(plan.tune);
    CHECK(plan.center_hz - kCentre == Approx(kStep * 9.0));
}

// Rejects an interval shorter than the retune costs. 330 ms is the measured
// dead stream on an R820T and the constant has to clear it, or every notch
// queues a stall behind the one before it and the coalescing accomplishes
// nothing. Asserted on the constant because it is the number the behaviour
// rests on, and a later edit lowering it for feel would be silent otherwise.
TEST_CASE("the settling interval clears the measured retune cost")
{
    CHECK(kScrollTuneSettleMs >= 330.0);

    ScrollTuneState state;
    ScrollTunePlan plan = plan_scroll_tune(state, wheel(120.0, 0.0));
    REQUIRE(plan.tune);
    state = plan.state;

    // One millisecond short of the interval is still held back, and the wait
    // it reports is the remainder rather than a whole fresh interval.
    plan = plan_scroll_tune(state, wheel(120.0, kScrollTuneSettleMs - 1.0));
    CHECK_FALSE(plan.tune);
    CHECK(plan.wait_ms == Approx(1.0));

    // A wait of zero would mean "nothing pending" to the caller and strand the
    // accumulator until the next wheel event.
    CHECK(plan.wait_ms > 0.0);
}

// Rejects an implementation that only ever adds. Scrolling back the other way
// is how an operator corrects an overshoot, and inside one interval the two
// gestures have to cancel: issuing the overshoot and then a correction is two
// retunes and two stalls to arrive where the radio already was.
TEST_CASE("scrolling back inside one interval cancels out")
{
    ScrollTuneState state;
    state.issued = true;
    state.issued_at_ms = 0.0;

    ScrollTunePlan plan = plan_scroll_tune(state, wheel(360.0, 10.0));
    CHECK_FALSE(plan.tune);
    state = plan.state;

    plan = plan_scroll_tune(state, wheel(-360.0, 20.0));
    CHECK_FALSE(plan.tune);
    state = plan.state;
    CHECK(state.pending_eighths == Approx(0.0));

    // The interval elapses with nothing left to spend, so nothing is issued
    // and nothing is waited for.
    plan = plan_scroll_tune(state, wheel(0.0, kScrollTuneSettleMs + 1.0));
    CHECK_FALSE(plan.tune);
    CHECK(plan.wait_ms == Approx(0.0));
}

// Rejects a target that walks off the end of what the source tunes.
// EngineLink refuses one and writes a fault line, so an operator sweeping up
// to the top of the band would be told they asked for something impossible by
// a gesture that has no way to ask a question.
TEST_CASE("the target is clamped into the source's range")
{
    ScrollTuneRequest request = wheel(120.0 * 40.0, 0.0, kTuneHigh - kStep);
    const ScrollTunePlan plan = plan_scroll_tune(ScrollTuneState{}, request);

    REQUIRE(plan.tune);
    CHECK(plan.center_hz == Approx(kTuneHigh));
}

// Rejects banking the travel that the clamp discarded. Forty notches into the
// top of the band and one notch back the other way should move the radio down
// by one notch, not unwind the thirty-nine the clamp swallowed.
TEST_CASE("travel spent against a limit is not banked")
{
    ScrollTuneRequest up = wheel(120.0 * 40.0, 0.0, kTuneHigh - kStep);
    ScrollTunePlan plan = plan_scroll_tune(ScrollTuneState{}, up);
    REQUIRE(plan.tune);
    REQUIRE(plan.center_hz == Approx(kTuneHigh));
    CHECK(plan.state.pending_eighths == Approx(0.0));

    plan = plan_scroll_tune(plan.state, wheel(-120.0, kScrollTuneSettleMs + 1.0, kTuneHigh));
    REQUIRE(plan.tune);
    CHECK(plan.center_hz == Approx(kTuneHigh - kStep));
}

// Rejects issuing a tune to the centre the radio is already on. At the end of
// the range every further notch clamps to the same number, and sending it
// costs the full 330 ms stall for a picture that does not move: the band edge
// would stutter the stream once per notch.
TEST_CASE("a clamped target equal to the current centre issues nothing")
{
    ScrollTuneState state;
    state.issued = true;
    state.issued_at_ms = 0.0;

    const ScrollTunePlan plan =
        plan_scroll_tune(state, wheel(120.0, kScrollTuneSettleMs + 1.0, kTuneHigh));

    CHECK_FALSE(plan.tune);
    CHECK(plan.wait_ms == Approx(0.0));
    CHECK(plan.state.pending_eighths == Approx(0.0));
}

// Rejects clamping against a range the source has not answered yet. Both
// bounds are zero until Session.sourceCanRetune has come back, and a clamp
// into [0, 0] would tune every wheel event to DC, which is the one frequency
// an R820T cannot be asked to lock. See core/source/rtlsdr_source.cpp.
TEST_CASE("an unanswered tuning range imposes no clamp")
{
    ScrollTuneRequest request = wheel(120.0, 0.0);
    request.tune_low_hz = 0.0;
    request.tune_high_hz = 0.0;

    const ScrollTunePlan plan = plan_scroll_tune(ScrollTuneState{}, request);
    REQUIRE(plan.tune);
    CHECK(plan.center_hz == Approx(kCentre + kStep));
}

// Rejects holding wheel that arrived before there was a picture. No span is
// the state before the first frame and after a source is closed, and a notch
// then has nothing to be scaled against. An implementation that banked it
// would move the radio off the back of a gesture the operator made minutes
// earlier, the first time a frame arrived.
TEST_CASE("no span tunes nothing and keeps nothing")
{
    ScrollTuneState state;
    state.pending_eighths = 600.0;

    ScrollTuneRequest request = wheel(120.0, 0.0);
    request.span_hz = 0.0;

    const ScrollTunePlan plan = plan_scroll_tune(state, request);
    CHECK_FALSE(plan.tune);
    CHECK(plan.wait_ms == Approx(0.0));
    CHECK(plan.state.pending_eighths == Approx(0.0));
}

// Rejects trusting the clock to move forwards. The caller hands over a
// monotonic reading so this should not happen, and the wrong answer if it ever
// does is the dangerous one: a negative elapsed time compared against the
// interval releases the accumulator immediately, which is the backlog this
// exists to prevent arriving all at once.
TEST_CASE("a clock reading that went backwards still holds the accumulator")
{
    ScrollTuneState state;
    state.issued = true;
    state.issued_at_ms = 1000.0;

    const ScrollTunePlan plan = plan_scroll_tune(state, wheel(120.0, 900.0));
    CHECK_FALSE(plan.tune);
    CHECK(plan.wait_ms > 0.0);
    CHECK(plan.state.pending_eighths == Approx(120.0));
}

// Rejects a flush that re-arms itself forever. The caller calls again with no
// wheel behind it, and on an empty accumulator that has to report nothing
// pending or the timer restarts on every expiry for the life of the window.
TEST_CASE("a flush with nothing accumulated asks for no further wait")
{
    ScrollTuneState state;
    state.issued = true;
    state.issued_at_ms = 0.0;

    const ScrollTunePlan plan = plan_scroll_tune(state, wheel(0.0, 10.0));
    CHECK_FALSE(plan.tune);
    CHECK(plan.wait_ms == Approx(0.0));
}

// Rejects adding the step to the frequency last asked for. A device with a
// tuning step rounds, so docs/rpc.md says every absolute frequency a client
// holds is stale when the call returns: the step goes on the centre the radio
// landed on, or a sweep accumulates the rounding error the whole way up the
// band.
TEST_CASE("the step is added to the centre handed in")
{
    ScrollTuneState state;
    state.issued = true;
    state.issued_at_ms = 0.0;

    // The radio landed 1234 Hz off what was asked for last time, which is what
    // the caller reads back off EngineLink::sourceCenterHz.
    const double landed = kCentre + kStep + 1234.0;
    const ScrollTunePlan plan =
        plan_scroll_tune(state, wheel(120.0, kScrollTuneSettleMs + 1.0, landed));

    REQUIRE(plan.tune);
    CHECK(plan.center_hz == Approx(landed + kStep));
}

// One notch is a whole notch, which is the arithmetic every case above rests
// on. Asserted directly so a change to either constant fails here naming the
// step rather than failing somewhere that reads as a coalescing fault.
TEST_CASE("one notch is one twentieth of the span")
{
    CHECK(kWheelNotchEighths == Approx(120.0));
    CHECK(kScrollTuneSpanFraction == Approx(1.0 / 20.0));
}
