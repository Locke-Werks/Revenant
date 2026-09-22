// The mouse wheel over a wide display, walking the SOURCE's centre along so a
// band can be swept by scrolling instead of typed into a box. One pure
// function, because everything here that can be wrong is arithmetic on a
// wheel delta, a span and two clock readings.
//
// WHAT THIS IS NOT. It does not move a receiver. The wheel over the
// fine-tuning display is a different gesture on a different surface,
// EngineLink::tuneReceiver against a stream that stays continuous across the
// change, and docs/ui-spectrum.md keeps the two apart for the reason the whole
// of this header exists: a retune of the front end is neither free nor
// instant, and a receiver move is both.
//
// WHY COALESCING IS THE WHOLE JOB
//
// A device retune costs about 330 ms with no samples at all, roughly 790,000
// of them at 2.4 MS/s, because rtlsdr_set_center_freq fails on a dongle that
// has been streaming for more than about half a second and the backend has to
// cancel the transfers around the tuner's control transfer. Measured
// 2026-09-21 on an R820T; docs/rpc.md, under "The front end can be pointed
// somewhere else", carries the measurement and the boundary.
//
// A wheel emits events far faster than that. Sent straight through, a second
// of scrolling queues a dozen retunes, each paying its own stall, and the
// radio is still working through them long after the operator stopped. So the
// wheel delta accumulates and at most one tune goes out per settling
// interval: the operator sees the picture jump in steps rather than watching a
// backlog drain. The waterfall smears across each step, which is what a
// discontinuous stream actually looks like and is the honest picture.
//
// WHY THIS HOLDS NO Qt. ui/tests links it, on the same terms as
// models/receiver_marker.h and render/history_resize.h. Which gestures issue a
// tune, which are held back, how a fractional notch adds up and what happens
// at the end of the tuning range are exactly the parts that go wrong, and none
// of them needs a window or a radio.

#pragma once

#include <algorithm>

namespace revenant::ui {

// Eighths of a degree in one detent of an ordinary mouse wheel, which is what
// QWheelEvent::angleDelta is denominated in. High-resolution wheels and
// touchpads send smaller values more often, which is why the accumulator is a
// double and nothing here rounds an event to a whole step: rounding down
// makes a touchpad do nothing, and rounding up makes it fly.
inline constexpr double kWheelNotchEighths = 120.0;

// How far one notch walks the centre, as a fraction of the span on screen.
//
// A FRACTION AND NOT A HERTZ COUNT. A step that suits a 2.4 MHz span is a
// rounding error on a 20 MS/s one, and a step that suits 20 MS/s throws a
// 2.4 MHz span most of the way across the display. The gesture has to feel the
// same on both, and what the operator is actually judging it against is the
// picture in front of them, which is the span.
//
// A twentieth, for two reasons pulling in opposite directions. One notch moves
// the picture by 5% of its width, about 60 pixels on a 1200 pixel display,
// which is unmistakable without anything on screen leaving it, and twenty
// notches walks a whole span, which is a second or two of ordinary scrolling.
// The upper bound is the span itself: a single step wider than the span means
// the spectrum between the two positions was never drawn, so a sweep can pass
// straight over a signal without it ever appearing.
//
// That bound is on the COALESCED step and not on one notch, and it is
// deliberately not enforced. A burst fast enough to accumulate twenty notches
// inside one interval is an operator travelling rather than sweeping, and
// capping the accumulator would make the same flick cover different distances
// depending on where the interval boundaries happened to fall.
inline constexpr double kScrollTuneSpanFraction = 1.0 / 20.0;

// The coalescing interval, in milliseconds, and it has a floor rather than a
// preference: below the measured 330 ms of dead stream the operator outruns
// the radio and every notch queues a stall behind the one before it.
//
// 400 rather than 330 because 330 is the pause alone. Around it sit a socket
// round trip, a supervisor thread that has to notice the request, and a retune
// the backend attempts up to four times because the first
// rtlsdr_set_center_freq after the cancel returned -9 every single time. Too
// long costs an operator some sweep rate, which they can see and work with;
// too short rebuilds the backlog this exists to prevent, which they cannot.
inline constexpr double kScrollTuneSettleMs = 400.0;

// What one display's wheel has accumulated, and when it last spent it.
//
// ONE OF THESE PER DISPLAY, which is not the same shape as the thing it is
// pacing. The settling interval is a property of the radio, so two displays
// scrolled inside one interval can still issue two tunes. It is per display
// because the alternative is a shared coalescer on EngineLink, and the gesture
// that would need it is moving the pointer between the spectrum and the
// waterfall mid-sweep inside 400 ms. EngineLink holds one pending-request slot
// that a later request overwrites, so even that case collapses rather than
// queueing without bound.
struct ScrollTuneState {
    // Wheel angle in eighths of a degree that has arrived and not yet been
    // turned into a tune. Signed: scrolling back the other way inside one
    // interval cancels out, which is what an operator overshooting and
    // correcting means by it.
    double pending_eighths = 0.0;

    // When the last tune went out, on the caller's monotonic clock. Only
    // meaningful with issued set; without it a reading of zero would be
    // indistinguishable from a tune issued at the moment the clock started.
    double issued_at_ms = 0.0;
    bool issued = false;
};

// One wheel event, or one timer expiry with no wheel behind it.
struct ScrollTuneRequest {
    // QWheelEvent::angleDelta resolved to one signed number. Zero is the
    // flush case: the caller's timer came due and is asking whether the
    // accumulator can be spent now.
    double angle_delta_eighths = 0.0;

    // The span the display is drawing, EngineLink::spanHighHz minus
    // spanLowHz. Zero or negative means there is no picture yet, so there is
    // nothing to scale a notch against and nothing to tune away from.
    double span_hz = 0.0;

    // Where the front end is now, EngineLink::sourceCenterHz. The step is
    // added to this rather than to the last target asked for, because a device
    // with a tuning step rounds and the landed centre is the only number that
    // says where the radio actually is. docs/rpc.md: "Every absolute frequency
    // a client is holding is stale when the call returns."
    double center_hz = 0.0;

    // What the source will take, EngineLink::sourceTuneLowHz and
    // sourceTuneHighHz. A pair that is not a range, which is every source that
    // has not answered yet, means no clamp here; EngineLink refuses an
    // out-of-range target anyway and the engine refuses it again.
    double tune_low_hz = 0.0;
    double tune_high_hz = 0.0;

    // A monotonic millisecond reading. Not a wall clock: this measures the
    // radio's recovery, and a wall clock that steps backwards over a
    // scrolling operator would release the whole backlog at once.
    double now_ms = 0.0;

    double settle_ms = kScrollTuneSettleMs;
};

struct ScrollTunePlan {
    // Retune the front end to center_hz now.
    bool tune = false;
    double center_hz = 0.0;

    // Milliseconds until the accumulator can be spent, or zero for nothing
    // held back. The caller arms a single-shot timer on this and calls again
    // with no wheel delta, which is what makes the last notch of a burst
    // arrive rather than sitting until the operator scrolls again.
    double wait_ms = 0.0;

    // To be stored back over the state that was passed in.
    ScrollTuneState state;
};

// QWheelEvent::angleDelta resolved to one signed number, positive for up in
// frequency.
//
// VERTICAL FIRST AND HORIZONTAL AS A FALLBACK, never the sum. A tilt wheel and
// a two-finger swipe both belong on this gesture, and a touchpad delivers a
// diagonal as both axes at once, where adding them would make a gesture that
// is mostly vertical tune further than it looks.
[[nodiscard]] inline double scroll_tune_eighths(double delta_x, double delta_y)
{
    return delta_y != 0.0 ? delta_y : delta_x;
}

// Whether this wheel event tunes, and where to.
//
// Nothing here decides whether the source can retune at all. That is
// EngineLink::sourceCanRetune, it is answered once per connection, and a file
// or a synthetic scene refuses in its own words; the caller gates on it so the
// wheel does nothing on those rather than posting a request that always fails.
[[nodiscard]] inline ScrollTunePlan plan_scroll_tune(const ScrollTuneState& state,
                                                     const ScrollTuneRequest& request)
{
    ScrollTunePlan out;
    out.state = state;

    // No span means no frame has arrived, or no source is open. A notch has
    // nothing to be scaled against, and holding it until one does would tune
    // the radio off the back of a gesture the operator has forgotten making.
    if (!(request.span_hz > 0.0)) {
        out.state.pending_eighths = 0.0;
        return out;
    }

    out.state.pending_eighths += request.angle_delta_eighths;
    if (out.state.pending_eighths == 0.0) {
        return out;
    }

    // Never tuned on this state, so the first notch goes out immediately and
    // the coalescing starts behind it. An interval imposed on the first notch
    // would put a 400 ms lag on every isolated scroll, which reads as the
    // wheel not working.
    if (state.issued) {
        const double elapsed = request.now_ms - state.issued_at_ms;
        if (elapsed < request.settle_ms) {
            // Never less than a millisecond. The remainder here is positive by
            // construction and can still be a fraction of nothing, and zero is
            // how this tells the caller there is nothing pending: a wait
            // rounded away would leave the accumulator holding wheel with no
            // timer armed to spend it.
            out.wait_ms = std::max(request.settle_ms - elapsed, 1.0);
            return out;
        }
    }

    const double notches = out.state.pending_eighths / kWheelNotchEighths;
    double target = request.center_hz + notches * request.span_hz * kScrollTuneSpanFraction;

    if (request.tune_high_hz > request.tune_low_hz) {
        target = std::clamp(target, request.tune_low_hz, request.tune_high_hz);
    }

    // Spent whether or not it moved anything. At the end of the range further
    // scrolling in the same direction otherwise accumulates against a clamp,
    // and the travel banked there would fire backwards the moment the operator
    // scrolled the other way.
    out.state.pending_eighths = 0.0;

    // Already there. A tune to the centre the radio is on still costs the full
    // stall, so the band edge stops quietly rather than stalling the stream
    // once per notch against a limit.
    if (target == request.center_hz) {
        return out;
    }

    out.tune = true;
    out.center_hz = target;
    out.state.issued = true;
    out.state.issued_at_ms = request.now_ms;
    return out;
}

}  // namespace revenant::ui
