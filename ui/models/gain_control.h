// A gain slider over one stage, as arithmetic.
//
// Pure and Qt-free so ui/tests can cover it, which is the same reason
// models/source_choice.h and models/receiver_marker.h are headers. What is here
// is only the mapping between a slider's position and a gain in decibels; the
// settling onto a step the device actually has is settle_gain in
// models/source_choice.h and is not repeated, because two answers to "which
// step is this" would part company at the ties and settle_gain's tie rule is
// load-bearing.
//
// WHY A FRACTION AND NOT DECIBELS ALL THE WAY THROUGH. A QML Slider is happiest
// with a continuous 0..1 and the stages are not continuous: an R820T reports 29
// discrete steps from 0 to 49.6 dB, unevenly spaced, and the gaps between them
// are not the same size. Driving the slider in decibels would put the handle
// where no step exists and then jump it, and driving it in step INDEX would make
// the handle move at a constant rate through unevenly spaced gains, so the
// control would feel wrong in the middle of its travel. A fraction of the
// stage's decibel range keeps the handle proportional to the thing the operator
// is judging, which is signal level, and the settle happens on the way out.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "core/rpc/types.h"
#include "models/source_choice.h"

namespace revenant::ui {

// Whether a stage can be driven at all. A stage whose ends are equal or
// crossed is a device reporting a control with no travel in it, and a slider
// over that is a control that does nothing.
[[nodiscard]] inline bool gain_stage_usable(const rpc::GainStage& stage)
{
    return stage.max_db > stage.min_db;
}

// The gain a slider at `fraction` is asking for, before the device rounds it.
//
// Clamped rather than refused outside 0..1, because a QML Slider can hand over
// a hair past its own ends from a drag that overshoots and refusing there would
// make the top and bottom of the travel unreachable.
[[nodiscard]] inline double gain_for_fraction(const rpc::GainStage& stage, double fraction)
{
    if (!gain_stage_usable(stage)) {
        return stage.min_db;
    }
    if (!std::isfinite(fraction)) {
        return stage.min_db;
    }

    const double clamped = std::clamp(fraction, 0.0, 1.0);
    return stage.min_db + clamped * (stage.max_db - stage.min_db);
}

// And back, for putting the handle where the device actually landed.
//
// THIS IS THE HALF THAT MATTERS FOR NOT LYING. The device answers a gain
// request with the step it took, which on a stepped stage is rarely what was
// asked, so the handle is placed from the ANSWER and not from the request. A
// slider that stayed where the pointer left it would show a gain the tuner is
// not on, and the whole reason set_source_gain returns a number is to make that
// impossible.
[[nodiscard]] inline double fraction_for_gain(const rpc::GainStage& stage, double db)
{
    if (!gain_stage_usable(stage) || !std::isfinite(db)) {
        return 0.0;
    }
    return std::clamp((db - stage.min_db) / (stage.max_db - stage.min_db), 0.0, 1.0);
}

// The step a slider should move by, as a fraction, so one arrow key or one
// wheel notch moves one step on a stepped stage.
//
// Zero for a continuous stage, which QML reads as "no stepping" and is right:
// a continuous stage has nothing to snap to and an invented step size would
// make part of its range unreachable from the keyboard.
//
// Derived from the COUNT of steps rather than from the distance between two of
// them, because they are unevenly spaced: on an R820T the gaps run from 0.9 dB
// to 4.2 dB, so any single gap used as the step size either skips stages at one
// end or takes several presses per step at the other. The count gives a key
// press one step on average everywhere, and settle_gain puts the request on a
// real step on the way out, so nothing lands between two.
[[nodiscard]] inline double gain_fraction_step(const rpc::GainStage& stage)
{
    if (!gain_stage_usable(stage) || stage.steps_db.size() < 2) {
        return 0.0;
    }
    return 1.0 / static_cast<double>(stage.steps_db.size() - 1);
}

// What a slider move should send: the fraction settled onto a step the device
// has. One call so a caller cannot do half of it.
[[nodiscard]] inline double gain_request_for_fraction(const rpc::GainStage& stage,
                                                      double fraction)
{
    return settle_gain(stage, gain_for_fraction(stage, fraction));
}

}  // namespace revenant::ui
