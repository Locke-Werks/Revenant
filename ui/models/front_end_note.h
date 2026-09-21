// What the front end is doing, said in words.
//
// WHY THIS EXISTS AT ALL
//
// Measured on air 2026-09-20, an RTL-SDR v3 at 95.1 MHz in an ordinary
// suburban FM environment: with gain=auto the detector reported three
// intermodulation products as real tracks at confidence 1.00. Setting gain to
// 20 improved the measured SNR of KKFM at 98.1 MHz by 5.7 dB and every
// phantom went. Every example in the tree used gain=auto, so the default path
// was the one that produced phantoms, and the client had nothing to say about
// it: three confident rows in the detection list is what an operator saw, and
// three confident rows is what a real band looks like.
//
// So core/detect/front_end.h measures whether the noise floor is following
// the strongest signal on the span, SourceStats carries the verdict, and the
// rule that turns it into a sentence is here rather than in the QML, on the
// argument models/source_pacing.h makes: the label cannot be wrong, the rule
// can, and a rule that fires on ordinary movement trains an operator to
// ignore the line.
//
// WHY IT HOLDS NO Qt. ui/tests links it, the same as source_pacing.h and
// frequency_entry.h. The decision is a switch over an enum and the sentence
// is a string, and both belong somewhere a test binary with no window can
// reach them.
//
// THE SENTENCE SAYS WHAT WAS SEEN AND NOT WHAT IS WRONG
//
// This is the whole discipline of this file and it is easy to lose. The
// measurement is a correlated floor lift. It is a symptom of front end
// compression and several other things produce it, which
// core/detect/front_end.h lists. A line reading "your front end is
// overloading" would be a diagnosis the engine cannot support, and the first
// time an operator chased it to a switching power supply instead they would
// stop believing the line. So the wording states the observation, names gain
// as the thing to try, and stops.
//
// NO HYSTERESIS HERE, unlike source_pacing.h, because the verdict arrives
// already smoothed: core/detect/front_end.h fits its slope over a ten second
// exponential window and carries its own enter and leave thresholds. Adding a
// second filter on top would make the rule a test drives not the rule the
// window runs.

#pragma once

#include <cstdint>
#include <string>

namespace revenant::ui {

// What the engine said about its front end this pass.
//
// Mirrors rpc::SourceStats' three fields rather than including the wire
// header, so ui/tests can drive this without linking the client. The caller
// copies them across, which is one line and is where a renumbering would be
// caught by the enum below not compiling.
struct FrontEndSample {
    // Ordinal for ordinal with rpc::FrontEndState and
    // detect::FrontEndVerdict. Kept as its own type for the reason above.
    enum class State : std::uint8_t { Unmeasured, Steady, SpanScales, FloorFollowsSignal };

    State state = State::Unmeasured;

    // Decibels of floor movement per decibel the strongest signal moved,
    // from the least-following part of the span.
    double slope = 0.0;

    // How far the span's mean floor sits above the quietest the engine's
    // monitor has seen.
    double floor_lift_db = 0.0;

    // The engine is driving its graph. A stopped engine measures nothing and
    // its last verdict is whatever it was when it stopped, so the sentence is
    // suppressed rather than frozen on screen. Same rule PacingSample takes.
    bool engine_running = false;
};

namespace detail {

// One decimal and a unit. std::to_string on a double gives six decimals and
// no way to ask for fewer without <format>, which would put a locale in the
// path of a number a test compares as a string. Same construction as
// source_pacing.h's factor_text and for the same reason.
[[nodiscard]] inline std::string one_decimal(double value)
{
    const bool negative = value < 0.0;
    const double magnitude = negative ? -value : value;

    const auto tenths = static_cast<std::int64_t>(magnitude * 10.0 + 0.5);
    std::string out = negative ? "-" : "";
    out += std::to_string(tenths / 10);
    out += '.';
    out += std::to_string(tenths % 10);
    return out;
}

}  // namespace detail

// The sentence, or empty when there is nothing to say.
//
// EMPTY FOR Unmeasured AND FOR Steady, which are the two ordinary states, so
// a healthy window carries no line. A status strip that always has a sentence
// in it is a status strip nobody reads.
//
// SpanScales GETS A SENTENCE and it is not a warning. The tuner's AGC moving
// the whole span is the thing that makes a waterfall breathe and a colour map
// hunt, and an operator who has not been told that is looking for a fault in
// the display. It also names the setting that causes it, which is the point
// of saying it at all.
[[nodiscard]] inline std::string front_end_sentence(const FrontEndSample& sample)
{
    if (!sample.engine_running) {
        return {};
    }

    // No default case; see cmake/CompilerFlags.cmake.
    switch (sample.state) {
        case FrontEndSample::State::Unmeasured:
        case FrontEndSample::State::Steady:
            return {};

        case FrontEndSample::State::SpanScales:
            return "the whole span's level is moving with the strongest signal on it, about "
                   "one decibel for one, which is a gain control changing. Set gain to a "
                   "number if it is on auto.";

        case FrontEndSample::State::FloorFollowsSignal:
            return "the noise floor across the whole span is rising " +
                   detail::one_decimal(sample.slope) +
                   " dB for every dB the strongest signal rises, and sits " +
                   detail::one_decimal(sample.floor_lift_db) +
                   " dB above the quietest it has been. A front end being driven too hard "
                   "does that: try a lower gain. So does a broadband interferer nearby.";
    }
    return {};
}

// Whether the sentence is bad news, which is the one thing the window decides
// differently from the text. SpanScales is a statement about a setting and
// FloorFollowsSignal is the one costing detections, and they must not be the
// same colour.
[[nodiscard]] inline bool front_end_is_fault(const FrontEndSample& sample)
{
    return sample.engine_running && sample.state == FrontEndSample::State::FloorFollowsSignal;
}

}  // namespace revenant::ui
