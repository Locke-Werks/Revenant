// Whether the receiver is the right shape for the signal it is on, and the
// one line that says so when it is not.
//
// TWO MISMATCHES BIT ON 2026-09-20 AND NEITHER SAID ANYTHING
//
// A click on a 145 kHz broadcast detection produced a receiver with a 16 kHz
// NFM passband. The filter was a ninth of the signal, the audio was a
// mangled fragment of the middle of it, and every number on screen was
// correct: the detection said 145 kHz, the passband readout said 16 kHz,
// and nothing put the two beside each other.
//
// A receiver asked for 200 kHz got 71 kHz, because the channel it landed in
// could not carry the rest. EngineLink already holds both the request and
// the grant, VrxPlacement::bandwidth_clamped is already true, and the
// passband overlay already draws the granted pair in a dimmer shade. What
// was missing was the sentence: an operator reading a spectrum does not
// notice a second shade of the same colour.
//
// Both are subtraction the window already had the inputs for. Doing it in
// words is the whole change.
//
// WHY THIS HOLDS NO Qt. ui/tests links it. Which comparisons fire, at what
// ratio, and how a width is written for a person are the parts that can be
// wrong, and all three are pure functions of five integers.

#pragma once

#include <cstdint>
#include <string>

namespace revenant::ui {

// The granted passband is called narrow for the signal when it is under
// this fraction of the detected bandwidth.
//
// 0.7 rather than 1.0 because a filter is SUPPOSED to be somewhat narrower
// than the occupied band the detector measures: the detector's bandwidth is
// where the energy is, skirts included, and a filter placed on the skirts
// passes noise. A tenth of the signal is not that, and 0.7 is comfortably
// above every sensible pairing in docs/modes.md while being far below the
// 0.11 the broadcast case produced.
inline constexpr double kNarrowFraction = 0.7;

// And wide when it is over this multiple of it. A filter three times the
// signal is passing two parts noise to one part audio, which is audible as
// hiss and is the other half of the same mistake: a WFM receiver left on a
// narrowband voice channel.
inline constexpr double kWideMultiple = 3.0;

// A filter narrower than this is not compared against a detection at all.
// CW is worked at a few hundred hertz against a carrier the detector
// measures as a couple of kilohertz wide, and that pairing is correct;
// flagging it would fire the line on every CW receiver ever tuned.
inline constexpr std::int64_t kIgnoreBelowHz = 1'000;

// What the window knows about the receiver and what put it there.
struct ReceiverFit {
    // Signed hertz from the receiver's centre, as VrxParams carries them.
    std::int64_t asked_low = 0;
    std::int64_t asked_high = 0;
    std::int64_t granted_low = 0;
    std::int64_t granted_high = 0;

    // VrxPlacement::bandwidth_clamped, read rather than re-derived. The
    // engine fits per edge, so a clamped receiver can be off-centre as well
    // as narrow and the widths alone do not always say a clamp happened.
    bool clamped = false;

    // The bandwidth of the detection this receiver was tuned from, in
    // hertz, or zero when it was not tuned from one. Zero is the ordinary
    // case for a receiver placed by hand or by a frequency typed into the
    // box, and it suppresses the signal comparison entirely: there is no
    // signal measurement to compare against and inventing one would be
    // worse than saying nothing.
    double detection_bandwidth_hz = 0.0;
};

enum class FitFlag : std::uint8_t {
    None = 0,

    // The channel could not carry what was asked for.
    Clamped = 1,

    // The granted filter is much narrower than the detected signal.
    NarrowForSignal = 2,

    // Much wider than it.
    WideForSignal = 4,
};

[[nodiscard]] constexpr std::uint8_t operator|(FitFlag a, FitFlag b)
{
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(a) |
                                     static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr bool has_flag(std::uint8_t flags, FitFlag flag)
{
    return (flags & static_cast<std::uint8_t>(flag)) != 0;
}

[[nodiscard]] constexpr std::int64_t asked_width(const ReceiverFit& fit)
{
    return fit.asked_high - fit.asked_low;
}

[[nodiscard]] constexpr std::int64_t granted_width(const ReceiverFit& fit)
{
    return fit.granted_high - fit.granted_low;
}

// Which of the three conditions hold. A bitmask and not an enum, because
// clamped and narrow-for-signal are independent and happen together: a
// 200 kHz request clamped to 71 kHz on a 145 kHz signal is both, and the
// operator needs both sentences rather than whichever one ranked higher.
[[nodiscard]] inline std::uint8_t classify_fit(const ReceiverFit& fit)
{
    std::uint8_t flags = 0;

    const std::int64_t asked = asked_width(fit);
    const std::int64_t granted = granted_width(fit);

    // Nothing to say before the engine has answered. A granted width of
    // zero is a receiver that has not been placed yet, not a filter of no
    // width.
    if (granted <= 0) {
        return flags;
    }

    // The flag OR the arithmetic, because they catch different things. The
    // flag is the engine's own answer and covers an edge pulled in without
    // the total width moving; the comparison covers an engine that granted
    // less than was asked without setting it. Neither alone is enough and
    // the disagreement is not worth a second sentence.
    if (fit.clamped || (asked > 0 && granted < asked)) {
        flags = flags | static_cast<std::uint8_t>(FitFlag::Clamped);
    }

    if (fit.detection_bandwidth_hz > 0.0 && granted >= kIgnoreBelowHz) {
        const auto width = static_cast<double>(granted);
        if (width < fit.detection_bandwidth_hz * kNarrowFraction) {
            flags = flags | static_cast<std::uint8_t>(FitFlag::NarrowForSignal);
        } else if (width > fit.detection_bandwidth_hz * kWideMultiple) {
            flags = flags | static_cast<std::uint8_t>(FitFlag::WideForSignal);
        }
    }

    return flags;
}

// A bandwidth as a person writes it. Integer arithmetic throughout, so a
// test compares the exact string rather than a rounding.
//
// Three decades and no more: hertz below a kilohertz, kilohertz below a
// megahertz, megahertz above. One decimal on the two scaled forms, dropped
// when it is zero, because "16 kHz" is what the operator said and
// "16.0 kHz" is what a formatter says.
[[nodiscard]] inline std::string format_width(std::int64_t hertz)
{
    const bool negative = hertz < 0;
    const std::int64_t magnitude = negative ? -hertz : hertz;
    const std::string sign = negative ? "-" : "";

    if (magnitude < 1'000) {
        return sign + std::to_string(magnitude) + " Hz";
    }

    const std::int64_t divisor = magnitude < 1'000'000 ? 1'000 : 1'000'000;
    const char* unit = magnitude < 1'000'000 ? " kHz" : " MHz";

    // Rounded to a tenth, half away from zero, in integers.
    const std::int64_t tenths = (magnitude * 10 + divisor / 2) / divisor;
    std::string out = sign + std::to_string(tenths / 10);
    if (tenths % 10 != 0) {
        out += '.';
        out += std::to_string(tenths % 10);
    }
    out += unit;
    return out;
}

// The line. Empty when the receiver fits, which is the ordinary case and
// has to leave no row behind.
//
// The two sentences are joined rather than ranked, because they are two
// different facts with two different fixes: a clamp is answered by a
// different grid or a different frequency, and a filter that is wrong for
// the signal is answered by dragging an edge or changing mode. Showing one
// and hiding the other sends the operator to the wrong one of those half
// the time.
[[nodiscard]] inline std::string fit_sentence(const ReceiverFit& fit)
{
    const std::uint8_t flags = classify_fit(fit);
    if (flags == 0) {
        return {};
    }

    std::string out;

    if (has_flag(flags, FitFlag::NarrowForSignal)) {
        out += format_width(granted_width(fit)) + " receiver on a " +
               format_width(static_cast<std::int64_t>(fit.detection_bandwidth_hz + 0.5)) +
               " detection: the filter is narrower than the signal.";
    } else if (has_flag(flags, FitFlag::WideForSignal)) {
        out += format_width(granted_width(fit)) + " receiver on a " +
               format_width(static_cast<std::int64_t>(fit.detection_bandwidth_hz + 0.5)) +
               " detection: the filter is far wider than the signal.";
    }

    if (has_flag(flags, FitFlag::Clamped)) {
        if (!out.empty()) {
            out += "  ";
        }

        // TWO SENTENCES, BECAUSE THE REQUEST IS NOT ALWAYS A NUMBER THIS
        // WINDOW HAS. A tune and a mode change both send an empty passband,
        // which is how the client asks for the mode's own default without
        // carrying a copy of the table, and EngineLink then adopts the
        // granted pair as the request. So on exactly the path that produces
        // the worst clamps, the asked width equals the granted one and the
        // engine's own flag is the only evidence. Printing "asked 71 kHz,
        // channel carries 71 kHz" there would read as a bug in this line.
        if (asked_width(fit) > 0 && asked_width(fit) != granted_width(fit)) {
            out += "asked " + format_width(asked_width(fit)) + ", channel carries " +
                   format_width(granted_width(fit)) + ".";
        } else {
            out += "the channel clamped this filter to " +
                   format_width(granted_width(fit)) + ".";
        }
    }

    return out;
}

}  // namespace revenant::ui
