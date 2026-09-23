// Automatic frequency tracking: the loop that nudges a receiver's centre so a
// drifting signal stays where the filter is.
//
// docs/ui-spectrum.md, "AFT", sets the rules and this is them, in the client:
//
//   Off by default. Nothing here moves a receiver until set_enabled(true).
//
//   A deadband, because a loop without one hunts, and a receiver walking
//   around its own signal while somebody tunes it is worse than no AFT.
//
//   A rate limit, and it yields to the operator. At most one move per
//   interval, each no larger than the slew allows. A measurement that moves
//   faster than drift does is a second signal or the operator, and in both
//   cases the answer is to stop rather than to chase, so it is refused and
//   the loop holds. Any tuning by hand holds the loop for a while and then
//   it starts again from wherever the receiver was left.
//
//   Holds still with no signal. A frame with nothing above the floor is not a
//   measurement of anything, and "not identified and nobody said" is the same:
//   a mode with no rule below never moves.
//
// WHAT IT AIMS AT, BY MODE. The doc's table, for the modes the engine has:
//
//   am        the carrier: the peak in the passband.
//   cw        the carrier, keyed: the peak, only while it is well above the
//             floor, holding through the gaps.
//   nfm, wfm  the centre of the deviation swing: a centroid of the occupied
//             band, averaged over seconds before it may move anything.
//   usb, lsb  the suppressed carrier IS the receiver's centre by definition,
//             so there is nothing to measure. Hold.
//   dsb       the same, a suppressed carrier. Hold.
//   raw       no demodulator, so no statement of what the signal is. Hold.
//
// RTTY AND FSK ARE NOT HERE. Their logical centre is the midpoint of two tones,
// derived from one tone and the shift, and the engine has neither an RTTY mode
// nor a shift parameter, so there is no state in which this loop could know it
// was on RTTY. A peak rule on RTTY jumps between mark and space and a centroid
// wobbles with the text, which is why neither is offered as a stand-in. The
// window says so where the toggle is.
//
// WHAT IT MEASURES FROM. The passband frames, which are after the receiver's
// filter. The filter's shape moves with the receiver, so a centroid includes
// some of it and is pulled towards the receiver's own centre: the error it
// reports is short of the true one, never of the wrong sign, and the loop
// converges over more moves rather than overshooting. Nothing here divides the
// filter out; a pre-filter tap is the engine's to add.
//
// Absolute hertz throughout, off each frame's own geometry, so a frame measured
// before a move landed still says where the signal is rather than where it was
// relative to a centre that has since changed.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string_view>

namespace revenant::ui {

enum class AftRule {
    Hold,
    Peak,
    KeyedPeak,
    Centroid,
};

// Mode names as core/engine/vrx.h spells them, which is what
// EngineLink::receiverDemod returns.
[[nodiscard]] constexpr AftRule aft_rule_for(std::string_view demod)
{
    if (demod == "am") {
        return AftRule::Peak;
    }
    if (demod == "cw") {
        return AftRule::KeyedPeak;
    }
    if (demod == "nfm" || demod == "wfm") {
        return AftRule::Centroid;
    }
    return AftRule::Hold;
}

struct AftSettings {
    // The smallest error the loop acts on. The frame's own bin width is the
    // other floor, since nothing finer than a bin is measured reliably.
    double deadband_hz = 30.0;

    // At most one move per this many seconds.
    double min_interval_s = 0.5;

    // How far the centre may be moved per second, so one move is at most
    // max_slew_hz_per_s times min_interval_s.
    double max_slew_hz_per_s = 200.0;

    // How fast a measurement may move before it is not drift. Oscillators
    // drift in hertz per second; a signal that moves by hundreds in a second
    // is another signal, or the operator.
    double max_drift_hz_per_s = 100.0;

    // The most drift allowed for across a gap, however long. Without a cap
    // the allowance grows while a jumped signal is refused, and after a few
    // seconds the loop would accept it and chase after all.
    double max_gap_drift_hz = 300.0;

    // How long tuning by hand holds the loop.
    double operator_hold_s = 3.0;

    // The averaging time of the centroid rule, and how long it must have
    // averaged before it may move anything.
    double centroid_tau_s = 2.0;

    // Below this, measured from the frame's floor, there is no signal.
    float min_snr_db = 10.0F;

    // Key-down for the CW rule: the carrier well clear of the floor.
    float key_down_snr_db = 15.0F;

    // The centroid weighs bins within this much of the loudest.
    float centroid_window_db = 12.0F;
};

// One passband frame, as the loop needs it.
struct AftFrame {
    std::span<const float> power_db;

    // Absolute hertz at the CENTRE of bin zero, and the width of a bin.
    double first_bin_hz = 0.0;
    double bin_hz = 0.0;

    // The frame's low percentile, which the display's floor is drawn from.
    float floor_db = 0.0F;

    // The receiver's filter, in absolute hertz. Only bins inside it are
    // measured: a stronger signal next door is not the one being tracked.
    double search_low_hz = 0.0;
    double search_high_hz = 0.0;
};

struct AftMeasurement {
    bool present = false;
    double hz = 0.0;
    float snr_db = 0.0F;
};

namespace detail {

struct BinRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

[[nodiscard]] inline BinRange search_bins(const AftFrame& frame)
{
    const std::size_t count = frame.power_db.size();
    if (count == 0 || !(frame.bin_hz > 0.0) || !(frame.search_high_hz > frame.search_low_hz)) {
        return {};
    }
    const double first = std::ceil((frame.search_low_hz - frame.first_bin_hz) / frame.bin_hz);
    const double last = std::floor((frame.search_high_hz - frame.first_bin_hz) / frame.bin_hz);
    const double top = static_cast<double>(count - 1);
    if (last < 0.0 || first > top || last < first) {
        return {};
    }
    return BinRange{static_cast<std::size_t>(std::max(first, 0.0)),
                    static_cast<std::size_t>(std::min(last, top)) + 1};
}

}  // namespace detail

// The loudest bin inside the filter, placed between bins by a parabola
// through it and its two neighbours. The parabola is on decibels, which for a
// windowed carrier is close to the main lobe's shape and puts the estimate
// within a small fraction of a bin; nearest-bin alone would make the deadband
// a bin wide whatever it was set to.
[[nodiscard]] inline AftMeasurement measure_peak(const AftFrame& frame)
{
    const detail::BinRange range = detail::search_bins(frame);
    if (range.end <= range.begin) {
        return {};
    }
    std::size_t best = range.begin;
    for (std::size_t i = range.begin + 1; i < range.end; ++i) {
        if (frame.power_db[i] > frame.power_db[best]) {
            best = i;
        }
    }

    double offset = 0.0;
    if (best > 0 && best + 1 < frame.power_db.size()) {
        const double left = frame.power_db[best - 1];
        const double mid = frame.power_db[best];
        const double right = frame.power_db[best + 1];
        const double curve = left - 2.0 * mid + right;
        if (curve < 0.0) {
            offset = std::clamp(0.5 * (left - right) / curve, -0.5, 0.5);
        }
    }

    AftMeasurement out;
    out.snr_db = frame.power_db[best] - frame.floor_db;
    out.hz = frame.first_bin_hz + (static_cast<double>(best) + offset) * frame.bin_hz;
    out.present = true;
    return out;
}

// The power-weighted centre of the bins within centroid_window_db of the
// loudest, inside the filter. Weights are linear power above that threshold,
// so the edge of the window contributes nothing and a bin wandering across it
// does not flick the answer.
[[nodiscard]] inline AftMeasurement measure_centroid(const AftFrame& frame, float window_db)
{
    const detail::BinRange range = detail::search_bins(frame);
    if (range.end <= range.begin) {
        return {};
    }
    float loudest = frame.power_db[range.begin];
    for (std::size_t i = range.begin + 1; i < range.end; ++i) {
        loudest = std::max(loudest, frame.power_db[i]);
    }
    const double threshold = std::pow(10.0, static_cast<double>(loudest - window_db) / 10.0);

    double weight = 0.0;
    double moment = 0.0;
    for (std::size_t i = range.begin; i < range.end; ++i) {
        const double power = std::pow(10.0, static_cast<double>(frame.power_db[i]) / 10.0);
        const double w = power - threshold;
        if (w > 0.0) {
            weight += w;
            moment += w * static_cast<double>(i);
        }
    }
    if (!(weight > 0.0)) {
        return {};
    }

    AftMeasurement out;
    out.snr_db = loudest - frame.floor_db;
    out.hz = frame.first_bin_hz + (moment / weight) * frame.bin_hz;
    out.present = true;
    return out;
}

enum class AftState {
    Off,
    NoRule,       // the mode gives no centre to aim at
    Yielding,     // the operator tuned a moment ago
    NoSignal,     // nothing above the floor in the filter
    KeyUp,        // CW between elements
    Averaging,    // the centroid has not averaged long enough to move
    Jumped,       // the measurement moved faster than drift; not chased
    Locked,       // inside the deadband
    Correcting,   // outside it, moving at the rate limit
};

[[nodiscard]] constexpr std::string_view aft_state_label(AftState state)
{
    switch (state) {
    case AftState::Off:
        return "off";
    case AftState::NoRule:
        return "no rule for this mode";
    case AftState::Yielding:
        return "yielding to you";
    case AftState::NoSignal:
        return "holding, no signal";
    case AftState::KeyUp:
        return "holding, key up";
    case AftState::Averaging:
        return "averaging";
    case AftState::Jumped:
        return "holding, signal jumped";
    case AftState::Locked:
        return "locked";
    case AftState::Correcting:
        return "correcting";
    }
    return "off";
}

struct AftStep {
    AftState state = AftState::Off;

    // Move the receiver's centre to centre_hz.
    bool move = false;
    double centre_hz = 0.0;

    // The signal's estimated distance from the receiver's centre, when there
    // is an estimate.
    bool have_error = false;
    double error_hz = 0.0;
};

class AftLoop {
public:
    AftLoop() = default;
    explicit AftLoop(const AftSettings& settings) : settings_(settings) {}

    [[nodiscard]] bool enabled() const { return enabled_; }

    // Turning it on starts a fresh acquisition; turning it off forgets.
    void set_enabled(bool on)
    {
        if (on != enabled_) {
            enabled_ = on;
            forget();
            holding_for_operator_ = false;
        }
    }

    // The operator tuned, retuned, dragged or changed mode. The loop holds
    // for operator_hold_s from now and then acquires afresh from wherever
    // the receiver is: what it was locked to before may not be what the
    // operator has just moved to.
    void operator_tuned(double now_s)
    {
        forget();
        hold_until_s_ = now_s + settings_.operator_hold_s;
        holding_for_operator_ = true;
    }

    // A different receiver, or none: nothing measured so far applies.
    //
    // THE OPERATOR'S HOLD SURVIVES THIS. A change of mode is tuning by hand
    // and is also a remove and an add underneath, so the first frame from
    // the new receiver arrives here moments after operator_tuned; clearing
    // the hold with the measurements ended it before it had begun.
    void forget()
    {
        have_reference_ = false;
        acquired_s_ = 0.0;
        reference_s_ = 0.0;
        reference_hz_ = 0.0;
        have_moved_ = false;
        last_move_s_ = 0.0;
    }

    // One frame. centre_hz is the receiver's centre in absolute hertz as the
    // client last asked for it, which is what a move is relative to.
    [[nodiscard]] AftStep step(double now_s, double centre_hz, AftRule rule,
                               const AftFrame& frame)
    {
        AftStep out;
        if (!enabled_) {
            out.state = AftState::Off;
            return out;
        }
        if (rule == AftRule::Hold) {
            forget();
            out.state = AftState::NoRule;
            return out;
        }
        if (holding_for_operator_) {
            if (now_s < hold_until_s_) {
                out.state = AftState::Yielding;
                return out;
            }
            holding_for_operator_ = false;
        }

        const AftMeasurement raw = rule == AftRule::Centroid
                                       ? measure_centroid(frame, settings_.centroid_window_db)
                                       : measure_peak(frame);
        if (!raw.present || raw.snr_db < settings_.min_snr_db) {
            out.state = AftState::NoSignal;
            return out;
        }
        if (rule == AftRule::KeyedPeak && raw.snr_db < settings_.key_down_snr_db) {
            out.state = AftState::KeyUp;
            return out;
        }

        // The measurement the loop acts on. The centroid rule averages;
        // the peak rules take each frame as it comes, because a carrier's
        // peak is already where it is.
        double estimate = raw.hz;
        if (!have_reference_) {
            have_reference_ = true;
            acquired_s_ = now_s;
            reference_s_ = now_s;
            reference_hz_ = raw.hz;
        } else {
            const double elapsed = std::max(now_s - reference_s_, 0.0);
            if (rule == AftRule::Centroid) {
                const double alpha = 1.0 - std::exp(-elapsed / settings_.centroid_tau_s);
                estimate = reference_hz_ + alpha * (raw.hz - reference_hz_);
            }

            // Two bins of slack for the measurement's own noise, plus what
            // drift could have done since the last accepted estimate.
            const double allowed =
                2.0 * frame.bin_hz +
                std::min(settings_.max_drift_hz_per_s * elapsed, settings_.max_gap_drift_hz);
            if (std::fabs(estimate - reference_hz_) > allowed) {
                out.state = AftState::Jumped;
                return out;
            }
            reference_s_ = now_s;
            reference_hz_ = estimate;
        }

        out.have_error = true;
        out.error_hz = estimate - centre_hz;

        if (rule == AftRule::Centroid && now_s - acquired_s_ < settings_.centroid_tau_s) {
            out.state = AftState::Averaging;
            return out;
        }

        const double deadband = std::max(settings_.deadband_hz, frame.bin_hz);
        if (std::fabs(out.error_hz) <= deadband) {
            out.state = AftState::Locked;
            return out;
        }

        out.state = AftState::Correcting;
        if (have_moved_ && now_s - last_move_s_ < settings_.min_interval_s) {
            return out;
        }
        const double largest = settings_.max_slew_hz_per_s * settings_.min_interval_s;
        const double correction = std::clamp(out.error_hz, -largest, largest);
        out.move = true;
        out.centre_hz = centre_hz + correction;
        have_moved_ = true;
        last_move_s_ = now_s;
        return out;
    }

private:
    AftSettings settings_{};
    bool enabled_ = false;

    bool holding_for_operator_ = false;
    double hold_until_s_ = 0.0;

    // The last accepted estimate, which is what a new one is checked
    // against for a jump, and when acquisition began.
    bool have_reference_ = false;
    double acquired_s_ = 0.0;
    double reference_s_ = 0.0;
    double reference_hz_ = 0.0;

    bool have_moved_ = false;
    double last_move_s_ = 0.0;
};

}  // namespace revenant::ui
