// The mouse wheel over the fine-tuning display, moving the RECEIVER's centre
// in steps sized to the display, coalesced so a fast wheel sends one move per
// interval rather than one per event.
//
// docs/ui-spectrum.md, "Horizontal scroll tunes": over the fine-tuning display
// the wheel moves the receiver, which is VrxParams::center through
// Engine::set_vrx_params, the path AFT uses and the one measured continuous
// across a change. It is not the front end's retune, which is
// models/scroll_tune.h and costs about 330 ms of dead stream; a receiver move
// is a push constant and a new tap table. So the interval here is short, a
// little more than a supervisor pass, and exists to keep a flick of the wheel
// from queueing a burst of requests the supervisor would collapse anyway.
//
// THE STEP IS A ROUND NUMBER OF HERTZ SIZED TO WHAT IS ON SCREEN. A fiftieth of
// the display's span, rounded down to 1, 2 or 5 times a power of ten: 200 Hz a
// notch on a 12 kHz nfm display, 5 kHz on a broadcast wfm one. A fraction of
// the span for the reason scroll_tune.h gives, that the gesture has to feel the
// same on every display; rounded, because the operator reads the dial after
// each notch and a receiver that walks in steps of 237 Hz lands on numbers
// nobody tunes to.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <cstdint>

#include "models/scroll_tune.h"

namespace revenant::ui {

inline constexpr double kReceiverScrollSpanDivisions = 50.0;
inline constexpr double kReceiverScrollSettleMs = 60.0;

// The step for one notch on a display this many hertz wide.
[[nodiscard]] constexpr std::int64_t receiver_scroll_step_hz(double span_hz)
{
    const double raw = span_hz / kReceiverScrollSpanDivisions;
    if (!(raw >= 1.0)) {
        return 1;
    }
    std::int64_t decade = 1;
    while (static_cast<double>(decade) * 10.0 <= raw) {
        decade *= 10;
    }
    if (static_cast<double>(decade) * 5.0 <= raw) {
        return decade * 5;
    }
    if (static_cast<double>(decade) * 2.0 <= raw) {
        return decade * 2;
    }
    return decade;
}

struct ReceiverScrollState {
    double pending_eighths = 0.0;
    double issued_at_ms = 0.0;
    bool issued = false;
};

struct ReceiverScrollRequest {
    // One wheel event resolved to a signed number, or zero for the flush.
    double angle_delta_eighths = 0.0;

    // The fine-tuning display's span, from its own frame. Zero is no frame
    // yet, which is nothing to scale a notch against.
    double span_hz = 0.0;

    // Where the receiver is now, absolute.
    double centre_hz = 0.0;

    // Where it may go: the source's span, since a receiver outside it is one
    // the engine removes. A pair that is not a range clamps nothing.
    double low_hz = 0.0;
    double high_hz = 0.0;

    double now_ms = 0.0;
    double settle_ms = kReceiverScrollSettleMs;
};

struct ReceiverScrollPlan {
    bool tune = false;
    double centre_hz = 0.0;
    double wait_ms = 0.0;
    ReceiverScrollState state;
};

// The same accumulate, first-notch-now, one-per-interval rule as
// plan_scroll_tune, with a step in hertz and a much shorter interval.
[[nodiscard]] inline ReceiverScrollPlan plan_receiver_scroll(const ReceiverScrollState& state,
                                                             const ReceiverScrollRequest& request)
{
    ReceiverScrollPlan out;
    out.state = state;

    if (!(request.span_hz > 0.0)) {
        out.state.pending_eighths = 0.0;
        return out;
    }

    out.state.pending_eighths += request.angle_delta_eighths;
    if (out.state.pending_eighths == 0.0) {
        return out;
    }

    if (state.issued) {
        const double elapsed = request.now_ms - state.issued_at_ms;
        if (elapsed < request.settle_ms) {
            out.wait_ms = std::max(request.settle_ms - elapsed, 1.0);
            return out;
        }
    }

    // Whole notches only. A touchpad's fractions stay banked until they add
    // up to one, because a receiver step is a round number of hertz and a
    // third of one is not.
    const auto notches =
        static_cast<std::int64_t>(out.state.pending_eighths / kWheelNotchEighths);
    if (notches == 0) {
        return out;
    }
    out.state.pending_eighths -= static_cast<double>(notches) * kWheelNotchEighths;

    const auto step = static_cast<double>(receiver_scroll_step_hz(request.span_hz));
    double target = request.centre_hz + static_cast<double>(notches) * step;
    if (request.high_hz > request.low_hz) {
        target = std::clamp(target, request.low_hz, request.high_hz);
    }
    if (target == request.centre_hz) {
        // Against the edge. Spent, so travel does not bank against a limit
        // and fire backwards later.
        out.state.pending_eighths = 0.0;
        return out;
    }

    out.tune = true;
    out.centre_hz = target;
    out.state.issued = true;
    out.state.issued_at_ms = request.now_ms;
    return out;
}

}  // namespace revenant::ui
