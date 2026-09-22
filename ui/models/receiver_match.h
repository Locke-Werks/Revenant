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

#include "core/rpc/types.h"

namespace revenant::ui {

// ---------------------------------------------------------------------------
// Which demodulator a measured signal wants
// ---------------------------------------------------------------------------
//
// The other half of the 145 kHz case this file's header describes. The
// sentence below the spectrum was one half: it said the filter did not fit
// the signal. This is the half that stops it happening, by choosing the
// mode from what the detector measured instead of leaving
// rpc::VrxParams::demod at its struct default.
//
// A COPY OF engine::demod_for_signal, AND A COPY ON PURPOSE, on the same
// terms as kDemodNames in models/engine_link.h: this process links no part
// of the engine, which is the whole reason ui/CMakeLists.txt exists. What
// keeps the two honest is that both derive the threshold from the same
// published fact rather than from each other, and that
// core/engine/vrx_place.cpp carries the argument and the list of what the
// rule gets wrong. Read it there before changing the number here.
//
// The engine's version takes a modulation family from core/characterise as
// well. This one does not, because nothing on the wire carries one: the
// characteriser reads complex baseband and the detector publishes a centre,
// a width and an SNR. When a family reaches a client, it arrives as a field
// on rpc::Detection and this function grows an argument.

// The widest occupied bandwidth that is still one narrowband FM channel.
//
// dsp::default_passband(rpc::Demod::Nfm) is +/-8 kHz and the engine derives
// it from land mobile in a 25 kHz channel: 5 kHz deviation plus 3 kHz of
// audio, doubled by Carson, is 16 kHz occupied inside a 25 kHz allocation.
// The channel rather than the Carson figure is the line, because a detector
// measures where the energy is and reports something between the two.
inline constexpr double kNarrowbandChannelHz = 25'000.0;

// The demodulator to open on a detection, from the bandwidth it measured.
//
// Zero or less is "no measurement", and gives back the same Nfm a caller
// that said nothing would have got. Above the narrowband channel is Wfm,
// which is deliberately the direction to be wrong in: a WFM receiver on a
// narrow signal passes the whole signal plus noise and sounds quiet, while
// an NFM receiver on a wide one truncates it and sounds broken at full
// strength.
[[nodiscard]] inline rpc::Demod demod_for_detection(double occupied_hz)
{
    if (!(occupied_hz > 0.0)) {
        return rpc::Demod::Nfm;
    }
    return occupied_hz > kNarrowbandChannelHz ? rpc::Demod::Wfm : rpc::Demod::Nfm;
}

// Whether a click on a detection gets to choose the demodulator, or has to
// leave the one the receiver already has.
//
// THE RULE THE WFM REVERT WAS MISSING. Deriving the mode from the measurement
// on every click overrides a mode the operator picked by hand exactly as
// readily as it overrides rpc::VrxParams::demod's Nfm default, and those are
// not the same value to be overriding. Reported from a live RTL-SDR on
// 2026-09-21 as the mode switching itself back to WFM: on broadcast FM every
// detection on the span is wider than kNarrowbandChannelHz, so pick nfm from
// the buttons, click the station again to nudge the receiver, and it is wfm
// again with nothing said.
//
//   `named` is true when the caller passed a mode, which is a statement the
//   detector cannot make and always wins.
//   `measured_hz` is the detection's occupied bandwidth, and zero or less is
//   no measurement, so there is nothing to derive from.
//   `operator_chose` is whether the operator has named a mode on THIS
//   receiver since it was created, which is the same rule the passband gets
//   for an edge they dragged by hand.
//
// A derived mode deliberately does not set `operator_chose` at the call site.
// Pinning the detector's own guess for the receiver's life would make the
// first click decide, which is the opposite of the point.
[[nodiscard]] inline bool click_chooses_demod(bool named, double measured_hz,
                                              bool operator_chose)
{
    if (named) {
        return false;
    }
    return measured_hz > 0.0 && !operator_chose;
}

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

// Build the fit from ONE status, so both halves of every comparison come off
// the same answer from the engine.
//
// echoed_low and echoed_high are VrxStatus::params' passband pair, which is
// a verbatim echo of the request the engine was given: the same reply
// carries the grant, so the two describe each other. granted_low and
// granted_high are VrxPlacement's.
//
// WHY THIS IS A FUNCTION AND NOT FIVE ASSIGNMENTS AT THE CALL SITE. Until
// 2026-09-21 EngineLink filled asked_low and asked_high from its own live
// request, which during a drag is the pair drawn on screen and not yet sent.
// Comparing that against the grant for the PREVIOUS request made every widen
// drag report a clamp: the operator pulls an edge out, the pane says "asked
// 40 kHz, channel carries 16 kHz", and the channel was never asked. A pan
// hid it, because a pan holds the width. Taking both numbers from the status
// makes the mistake unavailable rather than merely fixed.
//
// The cost is that the line lags by one poll after a real change, which is
// the right lag: until the engine answers, nobody knows what it granted.
[[nodiscard]] inline ReceiverFit fit_from_status(std::int64_t echoed_low,
                                                 std::int64_t echoed_high,
                                                 std::int64_t granted_low,
                                                 std::int64_t granted_high, bool clamped,
                                                 double detection_bandwidth_hz)
{
    ReceiverFit fit;
    fit.asked_low = echoed_low;
    fit.asked_high = echoed_high;
    fit.granted_low = granted_low;
    fit.granted_high = granted_high;
    fit.clamped = clamped;
    fit.detection_bandwidth_hz = detection_bandwidth_hz;
    return fit;
}

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
        // carrying a copy of the table. The status echoes that request
        // verbatim, so on exactly the path that produces the worst clamps
        // the asked pair is a pair of zeros and the engine's own flag is the
        // only evidence. Printing "asked 0 Hz, channel carries 71 kHz" there
        // would read as a bug in this line.
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
