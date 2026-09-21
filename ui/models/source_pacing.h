// Whether the SOURCE is keeping up, said in words.
//
// WHY THIS EXISTS AT ALL
//
// On 2026-09-20 a synthetic source running at 0.20x realtime produced
// chopped audio, and the operator spent twenty minutes in the audio path
// looking for it. Nothing on screen said the capture was behind. The audio
// strip said "starving", which is true and points at the sound card, and
// every counter it sits beside describes this machine. The one number that
// would have ended it in a glance is samples captured per wall second, and
// the engine has always known it.
//
// So EngineInfo carries realtimeFactor, and the rule that turns it into a
// sentence is here rather than in the QML, because it is the rule and not
// the label that can be wrong: a threshold that fires on ordinary
// measurement noise trains an operator to ignore the line, and one that
// never fires is the state this replaces.
//
// WHY IT HOLDS NO Qt. ui/tests links it. Same rule as
// render/history_resize.h and models/frequency_entry.h: the decision is
// arithmetic over three numbers and belongs somewhere a test binary with no
// window can reach it.
//
// A DELIBERATE 0.5 IS NOT A FAULT, WHICH IS WHY THERE ARE TWO INPUTS
//
// revenant-engine's --pace throttles a file or a synthetic source on
// purpose, and half speed is a thing people ask for. A factor of 0.5 means
// opposite things depending on whether anybody asked for it, so
// sourcePacedBy comes over the wire beside the measurement and 0 means
// unthrottled. Without it the only honest sentence would have been "the
// source is at 0.50x", which says nothing.
//
// HYSTERESIS, AND WHY THE FUNCTION TAKES ITS OWN PREVIOUS ANSWER
//
// The factor is a ratio of two counters over a finite window and it moves a
// percent or two on its own. A single threshold makes the line appear and
// disappear across that noise, which is the worst possible presentation of
// a warning: the operator learns it is decorative. So the verdict enters
// Behind below kBehindEnter and only leaves above kBehindLeave, and the
// caller hands back what it was told last time. That keeps the function
// pure, which is what lets a test drive the whole trajectory rather than
// one point on it.

#pragma once

#include <cstdint>
#include <string>

namespace revenant::ui {

// Below this the source is called behind. 0.97 rather than something
// rounder: at 0.97 a minute of capture is missing nearly two seconds, which
// is audible as clicks and visible as a waterfall that scrolls slower than
// the rate readout claims, and the measurement's own noise over a one
// second window is well inside the remaining three percent.
inline constexpr double kBehindEnter = 0.97;

// And it is only called caught up again above this. The gap between the two
// is the hysteresis; anything inside it keeps whatever the last answer was.
inline constexpr double kBehindLeave = 0.99;

// Above this the source is running faster than realtime, which is a file
// being read as fast as the disk allows and is not a fault. Well clear of
// kBehindLeave so the two bands cannot touch.
inline constexpr double kAheadEnter = 1.10;

// What the engine said about its source this pass.
struct PacingSample {
    // Samples of capture per wall second, divided by the source rate. 1.0
    // is realtime. False in `carried` means this engine's wire has no such
    // field, which is every engine built before the surface existed.
    double realtime_factor = 0.0;
    bool carried = false;

    // The --pace setting the source was started with, 0 for unthrottled.
    double paced_by = 0.0;

    // The engine is driving its graph. A stopped engine measures nothing
    // and its last factor is whatever it was when it stopped, so the
    // verdict is suppressed rather than frozen on screen: "engine stopped"
    // is already on the status line and is the better sentence.
    bool engine_running = false;
};

enum class PacingVerdict : std::uint8_t {
    // This engine does not carry the measurement.
    NotCarried,

    // It carries it and has not measured yet, or the engine is stopped.
    Unmeasured,

    // Unthrottled and keeping up.
    Realtime,

    // Unthrottled and behind. The one this whole header exists for.
    Behind,

    // Unthrottled and faster than realtime, which is a file replaying.
    Ahead,

    // Throttled on purpose and holding the pace it was given.
    Paced,

    // Throttled on purpose and not even making that.
    PacedAndBehind,
};

// The verdict, given what the engine said and what this was told last time.
// Pure: the previous verdict is the only state and the caller holds it.
[[nodiscard]] inline PacingVerdict classify_pacing(const PacingSample& sample,
                                                   PacingVerdict previous)
{
    if (!sample.carried) {
        return PacingVerdict::NotCarried;
    }
    if (!sample.engine_running || !(sample.realtime_factor > 0.0)) {
        return PacingVerdict::Unmeasured;
    }

    // Against the pace that was asked for, which is 1.0 when nobody asked
    // for one. The thresholds are ratios, so they hold at any pace: a
    // source asked for 0.25x and delivering 0.24x is 0.96 of its target and
    // is behind by the same rule a realtime source is.
    const bool throttled = sample.paced_by > 0.0;
    const double target = throttled ? sample.paced_by : 1.0;
    const double against_target = sample.realtime_factor / target;

    const bool was_behind =
        previous == PacingVerdict::Behind || previous == PacingVerdict::PacedAndBehind;
    const bool behind = was_behind ? against_target < kBehindLeave
                                   : against_target < kBehindEnter;

    if (behind) {
        return throttled ? PacingVerdict::PacedAndBehind : PacingVerdict::Behind;
    }
    if (throttled) {
        return PacingVerdict::Paced;
    }
    return against_target > kAheadEnter ? PacingVerdict::Ahead : PacingVerdict::Realtime;
}

namespace detail {

// Two decimals and an x, which is how a pace is written on the command
// line. std::to_string on a double gives six decimals and no way to ask for
// fewer without <format>, which MSVC has but which would put a locale in
// the path of a number a test compares as a string.
[[nodiscard]] inline std::string factor_text(double value)
{
    const bool negative = value < 0.0;
    const double magnitude = negative ? -value : value;

    // Half up at the hundredth, done in integers so the text is exactly
    // what the arithmetic says.
    const auto hundredths = static_cast<std::int64_t>(magnitude * 100.0 + 0.5);
    std::string out = negative ? "-" : "";
    out += std::to_string(hundredths / 100);
    out += '.';
    const std::int64_t rest = hundredths % 100;
    if (rest < 10) {
        out += '0';
    }
    out += std::to_string(rest);
    out += 'x';
    return out;
}

}  // namespace detail

// The sentence, or empty when there is nothing to say.
//
// EMPTY FOR Realtime AND FOR NotCarried, which are the two ordinary states,
// so a healthy window carries no line at all. A status strip that always
// has a sentence in it is a status strip nobody reads.
//
// THE BEHIND SENTENCE NAMES THE PLACE TO GO LOOKING, because that is the
// twenty minutes it is buying back. It says the capture is short and that
// the audio path is downstream of the shortfall, which is the inference the
// operator could not make from "starving".
[[nodiscard]] inline std::string pacing_sentence(PacingVerdict verdict,
                                                 const PacingSample& sample)
{
    // No default case; see cmake/CompilerFlags.cmake.
    switch (verdict) {
        case PacingVerdict::NotCarried:
        case PacingVerdict::Unmeasured:
        case PacingVerdict::Realtime:
            return {};

        case PacingVerdict::Behind:
            return "the source is behind: capture is running at " +
                   detail::factor_text(sample.realtime_factor) +
                   " realtime, so the gaps are upstream of the audio path.";

        case PacingVerdict::Ahead:
            return "capture is running at " + detail::factor_text(sample.realtime_factor) +
                   " realtime, which is a recording being read as fast as it can be.";

        case PacingVerdict::Paced:
            return "the source is paced at " + detail::factor_text(sample.paced_by) +
                   " on purpose and is holding it.";

        case PacingVerdict::PacedAndBehind:
            return "the source is behind: it was paced at " +
                   detail::factor_text(sample.paced_by) + " and is running at " +
                   detail::factor_text(sample.realtime_factor) +
                   ", so the gaps are upstream of the audio path.";
    }
    return {};
}

// Whether the sentence is bad news, which is the one thing the window
// decides differently from the text: Ahead and Paced are statements and
// Behind is a warning, and they must not be the same colour.
[[nodiscard]] inline bool pacing_is_fault(PacingVerdict verdict)
{
    return verdict == PacingVerdict::Behind || verdict == PacingVerdict::PacedAndBehind;
}

}  // namespace revenant::ui
