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
//   Holds still with no signal. A frame with nothing above the noise is not a
//   measurement of anything, and "not identified and nobody said" is the same:
//   a mode with no rule below never moves.
//
// WHAT IT AIMS AT, BY MODE. The doc's table, for the modes the engine has:
//
//   am        the carrier: the peak in the passband.
//   cw        the carrier, keyed: the peak while it clears the noise gate,
//             holding through the gaps.
//   nfm, wfm  the centre of the deviation swing: a centroid of the occupied
//             band, averaged over seconds before it may move anything.
//   usb, lsb  the suppressed carrier IS the receiver's centre by definition,
//             so there is nothing to measure. Hold.
//   dsb       the same, a suppressed carrier. Hold.
//   raw       no demodulator, so no statement of what the signal is. Hold.
//   p25p1, dstar, tetra, dmr
//             a decoder's complex tap. The P25, D-STAR and DMR decoders fit
//             the carrier's offset from their own sync words, and no rule
//             here has been measured on any of the four. Hold.
//
// RTTY AND FSK ARE NOT HERE. Their logical centre is the midpoint of two tones,
// derived from one tone and the shift, and the engine has neither an RTTY mode
// nor a shift parameter, so there is no state in which this loop could know it
// was on RTTY. A peak rule on RTTY jumps between mark and space and a centroid
// wobbles with the text, which is why neither is offered as a stand-in. The
// window says so where the toggle is.
//
// WHAT IT MEASURES FROM. The passband frames, which are the receiver's display
// tap: its exact mix and a fixed decimator, with none of its filter in them,
// so the noise floor is flat across the pane and nothing about the filter's
// shape pulls a measurement towards the receiver's centre. Only the bins
// inside the filter are searched, because the filter is what the operator
// said the signal is in and a neighbour beside it is not the one being
// tracked.
//
// WHAT THIS PARAGRAPH USED TO SAY: "The passband frames, which are after the
// receiver's filter", with the centroid "pulled towards the receiver's own
// centre" by the filter's shape, and "a pre-filter tap is the engine's to
// add". The engine added it in "Transform a display tap in the passband pane,
// not the fine ring".
//
// WHAT "ABOVE THE NOISE" MEANS, DERIVED RATHER THAN CHOSEN
//
// The gates used to compare against the frame's floor_db, a smoothed 5th
// percentile, with a fixed 10 dB for any signal and 15 dB for CW key-down. On
// the post-filter pane that percentile sat in the filter's stopband, so noise
// inside the filter read about 85 dB above it and the no-signal hold could
// never trigger. On the flat pane the same numbers still pass noise: one
// frame is one windowed transform with no averaging, so a noise bin's power
// is exponentially distributed, its 5th percentile is 12.90 dB under its
// mean, and the loudest of the 164 bins in a 6 kHz filter at 36.6 Hz sits
// about 7.5 dB over the mean. Noise alone read about 20 dB "above the floor"
// and cleared both.
//
// So three things replace the two constants.
//
//   The noise reference is the mean, taken from each frame's OWN 5th
//   percentile plus kPercentileToNoiseMeanDb, and averaged over noise_tau_s
//   of frames. The frame's own, the raw percentile_low_db, rather than the
//   smoothed floor, which falls in a frame and rises over thirty seconds and
//   so reads a noise that has just come up as a signal for half a minute.
//   Averaged, because one frame's percentile of 512 bins scatters by about
//   0.86 dB, and a gate that wanders by that much passes noise far more
//   often than it was sized for: at the 164-bin gate below, about 18% of
//   frames rather than 5%. Half a second is some eighteen frames and brings
//   the scatter under 0.2 dB. The percentile is taken across the whole pane,
//   which is noise unless signals cover 95% of it.
//
//   The per-frame gate is the level the loudest of N noise bins exceeds in
//   false_alarm of frames: P(max > t) is about N e^-t for N independent
//   exponential bins, so t = ln(N / p), in decibels over the mean. At the
//   shipped p = 0.05 that is 8.1 dB for 30 bins, 9.1 dB for 164 and 9.7 dB
//   for 600, never below min_snr_db. A carrier 10 dB over the noise clears it
//   in most frames and noise alone in one frame in twenty.
//
//   One frame in twenty is not a hold, so nothing is acted on until it has
//   been confirmed across confirm_windows distinct analysis windows: frames
//   whose windows overlap are one look at the air, not several, and a noise
//   peak persists across exactly those. A peak must reappear within two bins
//   of itself, which a noise peak does in about one gated frame in thirty on
//   a 164-bin filter; four agreeing windows is then roughly one false start
//   in several hours of noise. A centroid moves with the modulation, so it
//   confirms on gated frames with no more than centroid_max_gap_frames
//   missing between them, and its averaging holds it to the same rule for
//   centroid_tau_s before anything moves, which noise at one frame in twenty
//   does not survive.
//
// Absolute hertz throughout, off each frame's own geometry, so a frame measured
// before a move landed still says where the signal is rather than where it was
// relative to a centre that has since changed.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <array>
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

// The mean of exponentially distributed power over its 5th percentile, in
// decibels: -10 log10(-ln 0.95). A passband frame's bins are that
// distribution on noise, one windowed transform with no averaging, and its
// low percentile is the 5th (dsp::kSpectrumLowPermille, 50).
inline constexpr double kPercentileToNoiseMeanDb = 12.8995;

// The level the loudest of `bins` noise bins exceeds in `false_alarm` of
// frames, in decibels over the noise mean, and never below minimum_db. See
// the header: P(max > t) is about bins * e^-t.
[[nodiscard]] inline float noise_gate_db(std::size_t bins, double false_alarm, float minimum_db)
{
    if (bins == 0 || !(false_alarm > 0.0) || !(false_alarm < 1.0)) {
        return minimum_db;
    }
    const double t = std::log(static_cast<double>(bins) / false_alarm);
    if (!(t > 1.0)) {
        return minimum_db;
    }
    return std::max(minimum_db, static_cast<float>(10.0 * std::log10(t)));
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
    // averaged, confirmed throughout, before it may move anything.
    double centroid_tau_s = 2.0;

    // The fraction of noise-only frames whose loudest in-filter bin clears
    // the per-frame gate. See noise_gate_db.
    double false_alarm = 0.05;

    // The averaging time of the noise reference. See the header.
    double noise_tau_s = 0.5;

    // The gate never falls below this, in decibels over the noise mean,
    // which is where the approximation behind noise_gate_db stops holding
    // for a filter of a handful of bins.
    float min_snr_db = 6.0F;

    // Distinct analysis windows a measurement must be seen in before the loop
    // acts on it.
    int confirm_windows = 4;

    // A peak candidate not seen again for this long is dropped, so a noise
    // peak that happened to be first cannot hold the place of a carrier.
    double candidate_lapse_s = 1.0;

    // Ungated frames a centroid may miss in a row while it is confirming or
    // averaging before it starts again.
    int centroid_max_gap_frames = 2;

    // The centroid weighs bins within this much of the loudest...
    float centroid_window_db = 12.0F;

    // ...and at least this far over the noise mean, which is what makes the
    // band it is taken over the occupied band rather than the filter: a noise
    // bin clears 6 dB in about one frame in fifty.
    float occupied_db = 6.0F;

    // Bins under the threshold that may sit inside the occupied band before
    // it is taken to have ended. The band is the run of bins over the
    // threshold that contains the loudest, bridging gaps this wide, so an
    // isolated noise bin elsewhere in the filter is not part of it.
    int occupied_gap_bins = 4;
};

// One passband frame, as the loop needs it.
struct AftFrame {
    std::span<const float> power_db;

    // Absolute hertz at the CENTRE of bin zero, and the width of a bin.
    double first_bin_hz = 0.0;
    double bin_hz = 0.0;

    // This frame's own low percentile, the raw 5th, before any smoothing.
    // PassbandFrame::percentile_low_db.
    float percentile_low_db = 0.0F;

    // How long one frame's analysis window is, transform / rate. Frames closer
    // together than this overlap and are not independent looks. Zero counts
    // every frame as its own window.
    double window_s = 0.0;

    // The receiver's filter, in absolute hertz. Only bins inside it are
    // measured: a stronger signal next door is not the one being tracked.
    double search_low_hz = 0.0;
    double search_high_hz = 0.0;

    [[nodiscard]] float noise_db() const
    {
        return percentile_low_db + static_cast<float>(kPercentileToNoiseMeanDb);
    }
};

struct AftMeasurement {
    bool present = false;
    double hz = 0.0;

    // The loudest bin in the filter over the noise mean.
    float snr_db = 0.0F;

    // How many bins were searched, which is what the gate is sized by.
    std::size_t bins = 0;
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
//
// noise_db is the noise mean the level is read against; the loop passes its
// averaged one, and a caller holding one frame can pass frame.noise_db().
[[nodiscard]] inline AftMeasurement measure_peak(const AftFrame& frame, float noise_db)
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
    out.snr_db = frame.power_db[best] - noise_db;
    out.hz = frame.first_bin_hz + (static_cast<double>(best) + offset) * frame.bin_hz;
    out.bins = range.end - range.begin;
    out.present = true;
    return out;
}

[[nodiscard]] inline AftMeasurement measure_peak(const AftFrame& frame)
{
    return measure_peak(frame, frame.noise_db());
}

// The power-weighted centre of the OCCUPIED band: the run of bins inside the
// filter that contains the loudest and stays over the threshold, bridging
// gaps of occupied_gap_bins. The threshold is centroid_window_db under the
// loudest or occupied_db over the noise, whichever is higher. Weights are
// linear power above it, so the edge of the band contributes nothing and a
// bin wandering across it does not flick the answer.
//
// WHAT THIS USED TO WEIGH: every bin in the filter within the window of the
// loudest. On a strong signal that is the same band. On one ten or fifteen
// decibels up, the window reaches under the noise and every noise bin in the
// filter joins the centroid, which pulls it to the filter's centre and reads
// as an error of nothing.
[[nodiscard]] inline AftMeasurement measure_centroid(const AftFrame& frame,
                                                     const AftSettings& settings, float noise_db)
{
    const detail::BinRange range = detail::search_bins(frame);
    if (range.end <= range.begin) {
        return {};
    }
    std::size_t loudest_at = range.begin;
    for (std::size_t i = range.begin + 1; i < range.end; ++i) {
        if (frame.power_db[i] > frame.power_db[loudest_at]) {
            loudest_at = i;
        }
    }
    const float loudest = frame.power_db[loudest_at];
    const float threshold_db =
        std::max(loudest - settings.centroid_window_db, noise_db + settings.occupied_db);
    const double threshold = std::pow(10.0, static_cast<double>(threshold_db) / 10.0);

    // The occupied run, walked out from the loudest in both directions.
    const auto gap = static_cast<std::size_t>(std::max(settings.occupied_gap_bins, 0));
    std::size_t low = loudest_at;
    std::size_t missed = 0;
    for (std::size_t i = loudest_at; i > range.begin;) {
        --i;
        if (frame.power_db[i] > threshold_db) {
            low = i;
            missed = 0;
        } else if (++missed > gap) {
            break;
        }
    }
    std::size_t high = loudest_at;
    missed = 0;
    for (std::size_t i = loudest_at + 1; i < range.end; ++i) {
        if (frame.power_db[i] > threshold_db) {
            high = i;
            missed = 0;
        } else if (++missed > gap) {
            break;
        }
    }

    double weight = 0.0;
    double moment = 0.0;
    for (std::size_t i = low; i <= high; ++i) {
        const double power = std::pow(10.0, static_cast<double>(frame.power_db[i]) / 10.0);
        const double w = power - threshold;
        if (w > 0.0) {
            weight += w;
            moment += w * static_cast<double>(i);
        }
    }

    AftMeasurement out;
    out.snr_db = loudest - noise_db;
    out.bins = range.end - range.begin;
    if (!(weight > 0.0)) {
        // Nothing clears the threshold, which only a frame with nothing in it
        // does: present, at the loudest, and far below any gate.
        out.hz = frame.first_bin_hz + static_cast<double>(loudest_at) * frame.bin_hz;
    } else {
        out.hz = frame.first_bin_hz + (moment / weight) * frame.bin_hz;
    }
    out.present = true;
    return out;
}

[[nodiscard]] inline AftMeasurement measure_centroid(const AftFrame& frame,
                                                     const AftSettings& settings)
{
    return measure_centroid(frame, settings, frame.noise_db());
}

enum class AftState {
    Off,
    NoRule,       // the mode gives no centre to aim at
    Yielding,     // the operator tuned a moment ago
    NoSignal,     // nothing clears the noise gate in the filter
    Acquiring,    // something does, and it has not been confirmed yet
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
    case AftState::Acquiring:
        return "acquiring";
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

    // The averaged noise mean the gates are read against, in decibels, once
    // a frame has been seen.
    [[nodiscard]] bool have_noise() const { return have_noise_; }
    [[nodiscard]] float noise_db() const
    {
        return static_cast<float>(10.0 * std::log10(noise_power_));
    }

    // Turning it on starts a fresh acquisition; turning it off forgets.
    void set_enabled(bool on)
    {
        if (on != enabled_) {
            enabled_ = on;
            forget();
            holding_for_operator_ = false;
            have_noise_ = false;
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
        drop_candidate();
        forget_recent();
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

        // Every frame feeds the noise average, including the ones the
        // operator's hold ignores, so the reference is ready when it ends.
        // Averaged as power: the percentile's scatter is close to symmetric
        // in power and not in decibels.
        const double measured_noise =
            std::pow(10.0, static_cast<double>(frame.noise_db()) / 10.0);
        if (!have_noise_) {
            have_noise_ = true;
            noise_power_ = measured_noise;
        } else {
            const double dt = std::max(now_s - noise_s_, 0.0);
            const double alpha = 1.0 - std::exp(-dt / settings_.noise_tau_s);
            noise_power_ += alpha * (measured_noise - noise_power_);
        }
        noise_s_ = now_s;

        if (holding_for_operator_) {
            if (now_s < hold_until_s_) {
                out.state = AftState::Yielding;
                return out;
            }
            holding_for_operator_ = false;
        }

        const float noise = noise_db();
        const AftMeasurement raw = rule == AftRule::Centroid
                                       ? measure_centroid(frame, settings_, noise)
                                       : measure_peak(frame, noise);
        const bool gated =
            raw.present &&
            raw.snr_db >= noise_gate_db(raw.bins, settings_.false_alarm, settings_.min_snr_db);

        if (!gated) {
            ++missed_frames_;
            // A centroid that has not yet earned a move starts again after
            // a gap: that is what noise, gated one frame in twenty, does.
            if (rule == AftRule::Centroid && missed_frames_ > settings_.centroid_max_gap_frames &&
                (have_candidate_ || (have_reference_ && averaging(now_s)))) {
                have_reference_ = false;
                drop_candidate();
            }
            out.state = rule == AftRule::KeyedPeak && have_reference_ ? AftState::KeyUp
                                                                      : AftState::NoSignal;
            return out;
        }
        missed_frames_ = 0;

        // Confirmation, before anything is acted on. See the header.
        if (!have_reference_) {
            if (!confirm(now_s, rule, raw.hz, frame)) {
                out.state = AftState::Acquiring;
                return out;
            }
            have_reference_ = true;
            acquired_s_ = now_s;
            reference_s_ = now_s;
            reference_hz_ = candidate_hz_;
            drop_candidate();
            forget_recent();
        }

        // The measurement the loop acts on. The centroid rule averages over
        // seconds. The peak rules take the median of the last few frames:
        // a carrier's peak is already where it is, but ten decibels over the
        // flat floor, one frame in a few hundred has a noise bin beside the
        // carrier outshining it, and acting on that one frame was a 65 Hz
        // move away from a carrier the loop was locked to.
        double estimate = raw.hz;
        const double elapsed = std::max(now_s - reference_s_, 0.0);
        if (rule == AftRule::Centroid) {
            const double alpha = 1.0 - std::exp(-elapsed / settings_.centroid_tau_s);
            estimate = reference_hz_ + alpha * (raw.hz - reference_hz_);
        }

        // Two bins of slack for the measurement's own noise, plus what
        // drift could have done since the last accepted estimate.
        //
        // ONE FRAME OUTSIDE IT IS AN OUTLIER AND TWO IN A ROW ARE A JUMP. On
        // the flat pane a weak carrier is outshone now and then by a noise
        // bin elsewhere in the filter, and calling each of those a jump
        // flashed the warning on a loop that was tracking perfectly well. The
        // outlier is not acted on either way; it only does not change what
        // the loop says it is doing.
        if (std::fabs(estimate - reference_hz_) > allowed_hz(elapsed, frame)) {
            ++outliers_;
            if (outliers_ >= 2) {
                out.state = AftState::Jumped;
                return out;
            }
            out = last_;
            out.move = false;
            return out;
        }
        outliers_ = 0;
        if (rule != AftRule::Centroid) {
            estimate = recent_median(raw.hz);
        }
        reference_s_ = now_s;
        reference_hz_ = estimate;

        out.have_error = true;
        out.error_hz = estimate - centre_hz;

        if (rule == AftRule::Centroid && averaging(now_s)) {
            out.state = AftState::Averaging;
            last_ = out;
            return out;
        }

        const double deadband = std::max(settings_.deadband_hz, frame.bin_hz);
        if (std::fabs(out.error_hz) <= deadband) {
            out.state = AftState::Locked;
            last_ = out;
            return out;
        }

        out.state = AftState::Correcting;
        last_ = out;
        if (have_moved_ && now_s - last_move_s_ < settings_.min_interval_s) {
            return out;
        }
        const double largest = settings_.max_slew_hz_per_s * settings_.min_interval_s;
        const double correction = std::clamp(out.error_hz, -largest, largest);
        out.move = true;
        out.centre_hz = centre_hz + correction;
        have_moved_ = true;
        last_move_s_ = now_s;

        // The estimates behind the median are kept across the move: they are
        // in absolute hertz, so moving the receiver does not stale them.
        return out;
    }

private:
    // Two bins of slack for the measurement's own noise, plus what drift
    // could have done in `elapsed` seconds, capped.
    [[nodiscard]] double allowed_hz(double elapsed, const AftFrame& frame) const
    {
        return 2.0 * frame.bin_hz +
               std::min(settings_.max_drift_hz_per_s * elapsed, settings_.max_gap_drift_hz);
    }

    // The median of the last few accepted peak estimates, this one included.
    [[nodiscard]] double recent_median(double hz)
    {
        recent_[recent_next_] = hz;
        recent_next_ = (recent_next_ + 1) % recent_.size();
        recent_count_ = std::min(recent_count_ + 1, recent_.size());
        std::array<double, kRecentPeaks> sorted{};
        std::copy_n(recent_.begin(), recent_count_, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(recent_count_));
        return sorted[recent_count_ / 2];
    }

    void forget_recent()
    {
        recent_next_ = 0;
        recent_count_ = 0;
        outliers_ = 0;
        last_ = {};
    }

    [[nodiscard]] bool averaging(double now_s) const
    {
        return now_s - acquired_s_ < settings_.centroid_tau_s;
    }

    void drop_candidate()
    {
        have_candidate_ = false;
        candidate_hz_ = 0.0;
        candidate_seen_s_ = 0.0;
        candidate_counted_s_ = 0.0;
        candidate_windows_ = 0;
        missed_frames_ = 0;
    }

    // Counts one gated measurement towards confirmation and says whether it
    // is now confirmed. A peak counts only where it agrees with the candidate;
    // a centroid counts wherever it is, since it moves with the modulation,
    // and the gap rule in step() is what holds it to continuity instead.
    [[nodiscard]] bool confirm(double now_s, AftRule rule, double hz, const AftFrame& frame)
    {
        const bool peak = rule != AftRule::Centroid;
        if (have_candidate_ && peak && now_s - candidate_seen_s_ > settings_.candidate_lapse_s) {
            drop_candidate();
        }
        if (!have_candidate_) {
            have_candidate_ = true;
            candidate_hz_ = hz;
            candidate_seen_s_ = now_s;
            candidate_counted_s_ = now_s;
            candidate_windows_ = 1;
            return candidate_windows_ >= settings_.confirm_windows;
        }

        if (peak) {
            const double allowed = 2.0 * frame.bin_hz +
                                   settings_.max_drift_hz_per_s * (now_s - candidate_seen_s_);
            if (std::fabs(hz - candidate_hz_) > allowed) {
                // Not the candidate. Ignored rather than taken as the new
                // one, so a carrier being confirmed is not knocked back to
                // the start by the one frame in twenty a noise peak outshines
                // it; the lapse above is what retires a candidate that was
                // noise.
                return false;
            }
        }
        candidate_hz_ = hz;
        candidate_seen_s_ = now_s;

        // A new window only once the last counted one has passed: frames
        // closer than a window are the same look at the air.
        if (now_s - candidate_counted_s_ >= frame.window_s) {
            ++candidate_windows_;
            candidate_counted_s_ = now_s;
        }
        return candidate_windows_ >= settings_.confirm_windows;
    }

    AftSettings settings_{};
    bool enabled_ = false;

    bool holding_for_operator_ = false;
    double hold_until_s_ = 0.0;

    // A measurement on its way to being confirmed.
    bool have_candidate_ = false;
    double candidate_hz_ = 0.0;
    double candidate_seen_s_ = 0.0;
    double candidate_counted_s_ = 0.0;
    int candidate_windows_ = 0;
    int missed_frames_ = 0;

    // The last accepted estimate, which is what a new one is checked
    // against for a jump, and when acquisition began.
    bool have_reference_ = false;
    double acquired_s_ = 0.0;
    double reference_s_ = 0.0;
    double reference_hz_ = 0.0;

    bool have_moved_ = false;
    double last_move_s_ = 0.0;

    // The last few accepted peak estimates, for the median. Five: long enough
    // that one outshone frame never decides it, short enough that a carrier
    // drifting at the jump gate's own rate is followed within three frames.
    static constexpr std::size_t kRecentPeaks = 5;
    std::array<double, kRecentPeaks> recent_{};
    std::size_t recent_next_ = 0;
    std::size_t recent_count_ = 0;

    // Frames in a row whose measurement fell outside the jump allowance, and
    // what the loop last said while it was acting, which an outlier repeats.
    int outliers_ = 0;
    AftStep last_{};

    // The averaged noise mean, as linear power, and when it was last fed.
    // Kept across forget(): it is a property of the band, not of what the
    // loop was locked to.
    bool have_noise_ = false;
    double noise_power_ = 1.0;
    double noise_s_ = 0.0;
};

}  // namespace revenant::ui
