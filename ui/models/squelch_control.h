// The receiver panel's squelch slider: what a position on it sends, and what
// a threshold the receiver already has puts it at.
//
// The engine gates a receiver's audio on VrxParams::squelch_dbfs, compared
// against the same level the receiver's meter shows (core/engine/graph.cpp).
// Its default, -200 dBFS, is under the arithmetic's own floor, so a receiver
// built from the defaults is permanently open. The slider's bottom stop is
// that default rather than a number: an operator who pulls it all the way
// down means "no squelch", and a threshold of -120 dBFS is a squelch that
// shuts on digital silence, which is not the same thing.
//
// The range covers the rack meter's sixty decibels, -80 to -20 dBFS
// (models/level_meter.h), with room under it for a narrow receiver's noise
// floor on a quiet band and over it for a strong local station. A threshold
// set elsewhere, from the command line or before this existed, outside the
// range pins the handle at an end and is shown as the number it is.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace revenant::ui {

// VrxParams::squelch_dbfs's default: the gate never shuts.
inline constexpr double kSquelchOpenDbfs = -200.0;

// The slider's ends. The bottom one is the open position and sends
// kSquelchOpenDbfs; the lowest threshold it sends is one step above it.
inline constexpr double kSquelchSliderLowDbfs = -120.0;
inline constexpr double kSquelchSliderHighDbfs = -10.0;
inline constexpr double kSquelchStepDb = 1.0;

// Whether a threshold is the permanently open one. Anything at or under the
// engine's default is: the level never reads below it.
[[nodiscard]] constexpr bool squelch_is_open_setting(double dbfs)
{
    return !(dbfs > kSquelchOpenDbfs);
}

// A threshold as it goes to the engine: NaN and anything at or under the
// default become the default, and a number is kept to the step.
[[nodiscard]] inline double normalise_squelch(double dbfs)
{
    if (squelch_is_open_setting(dbfs)) {
        return kSquelchOpenDbfs;
    }
    return std::round(dbfs / kSquelchStepDb) * kSquelchStepDb;
}

// What a slider position sends. The bottom stop, and anything within half a
// step of it, is open; the rest is clamped to the range and rounded.
[[nodiscard]] inline double squelch_from_slider(double position)
{
    if (!(position >= kSquelchSliderLowDbfs + 0.5 * kSquelchStepDb)) {
        return kSquelchOpenDbfs;
    }
    return normalise_squelch(std::min(position, kSquelchSliderHighDbfs));
}

// Where the handle sits for a threshold. Open, and any threshold under the
// range, is the bottom stop.
[[nodiscard]] inline double slider_from_squelch(double dbfs)
{
    if (squelch_is_open_setting(dbfs) || dbfs < kSquelchSliderLowDbfs) {
        return kSquelchSliderLowDbfs;
    }
    return std::min(dbfs, kSquelchSliderHighDbfs);
}

// The threshold in words beside the slider: "open", or "-73 dBFS".
[[nodiscard]] inline std::string squelch_text(double dbfs)
{
    if (squelch_is_open_setting(dbfs)) {
        return "open";
    }
    char text[32];
    std::snprintf(text, sizeof text, "%.0f dBFS", normalise_squelch(dbfs));
    return text;
}

// What the gate indication says. Nothing while the squelch is open, because
// a gate that cannot shut has nothing to report, and nothing before the
// receiver's first status, whose squelch_open defaults to false and would
// otherwise read as shut. Then "shut" or "open", the engine's own words for
// the gate on the command line.
[[nodiscard]] inline std::string squelch_gate_text(double threshold_dbfs, bool reported,
                                                   bool gate_open)
{
    if (squelch_is_open_setting(threshold_dbfs) || !reported) {
        return {};
    }
    return gate_open ? "open" : "shut";
}

}  // namespace revenant::ui
