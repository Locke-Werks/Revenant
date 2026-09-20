#include "core/decode/rds_bits.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <format>
#include <numbers>

namespace revenant::decode {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;

// Working rate the decimator aims for. Sixteen samples per bit leaves eight
// per half-bit, which is where cubic interpolation of a signal band-limited to
// 2375 Hz stops being the dominant error: at eight samples per half-symbol the
// interpolation residue is around 60 dB down, far below any noise floor a
// decoder is used at, and at four it is around 40 dB and starts showing up in
// the timing loop as jitter.
constexpr double kTargetWorkingRateHz = 16.0 * kBitRateHz;

// Stopband attenuation of the decimating anti-alias filter.
//
// 80 dB rather than the usual 60 because of what the filter has to kill. The
// 19 kHz pilot sits 38 kHz below the subcarrier after mixing, and 38 kHz
// aliases straight onto DC at a 19 kHz working rate. The pilot runs around
// 9 percent of peak deviation against the subcarrier's 2, so it arrives about
// 13 dB ABOVE the wanted signal and lands exactly where the data is.
constexpr double kDecimatorStopbandDb = 80.0;

// Truncation span of the clause 1.7 shaping filter, in bit periods either
// side. The impulse response falls as 1/t^2, so five bit periods is 60 dB
// down and the residue is below the decimator's own stopband.
constexpr std::size_t kShapingSpanBits = 5;

// Kaiser window on the truncated shaping response. Without it the rectangular
// truncation leaves sidelobes around 50 dB down, which is not enough to hold
// off the pilot residue the decimator left. beta 4 costs a few percent of
// passband width against HT(f)'s ideal shape and buys 30 dB of stopband.
constexpr double kShapingKaiserBeta = 4.0;

// The two pilot measurement bandwidths, in hertz. Ten to one, which is what
// separates a tone from noise on the ratio of their outputs.
constexpr double kPilotWideHz = 250.0;
constexpr double kPilotNarrowHz = 25.0;

// Candidate bit phases the acquisition scan tries across one bit period.
// Thirty-two is Tb/32, about a fortieth of the eye, which is well inside the
// tracking loop's pull-in range.
constexpr std::size_t kScanPhases = 32;

// Early-late half-gate is not used: timing tracking is the mid-bit zero
// crossing. This is the linearisation gain of that detector, the slope of the
// filtered waveform through the crossing expressed against the decision
// statistic. Derived rather than tuned: the waveform swings from +A to -A
// across half a bit period, so its slope at the crossing is about 4A per bit
// period, and the statistic is 2A. A loop is insensitive to a factor of two
// here, which only rescales its bandwidth.
constexpr double kZeroCrossingGain = 4.0;

// Loop damping. 1/sqrt(2) everywhere, which is the standard choice and the
// one the coefficient formulas below are quoted against.
constexpr double kDamping = 0.70710678118654752;

struct LoopGains {
    double proportional = 0.0;
    double integral = 0.0;
};

// Second order loop coefficients for a given noise bandwidth, both normalised
// so the detector is expected to hand back its error in the same units the
// loop integrates. Standard digital PLL design; nothing specification-derived
// about it.
[[nodiscard]] LoopGains loop_gains(double bandwidth_times_period)
{
    const double theta = bandwidth_times_period / (kDamping + 0.25 / kDamping);
    const double denom = 1.0 + 2.0 * kDamping * theta + theta * theta;
    return LoopGains{4.0 * kDamping * theta / denom, 4.0 * theta * theta / denom};
}

// Modified Bessel function of the first kind, order zero, by its power series.
//
// core/dsp/synth/channel.cpp has one too, file-local for the same reason this
// one is: it is ten lines, it is only wanted for a Kaiser window, and exposing
// it would put a numerical special function in a public header that nothing
// else asks for.
[[nodiscard]] double bessel_i0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        const double half = x / (2.0 * static_cast<double>(k));
        term *= half * half;
        sum += term;
        if (term < 1e-18 * sum) {
            break;
        }
    }
    return sum;
}

[[nodiscard]] double kaiser(std::size_t index, std::size_t length, double beta)
{
    if (length <= 1) {
        return 1.0;
    }
    const double n = static_cast<double>(index);
    const double m = static_cast<double>(length - 1);
    const double ratio = 2.0 * n / m - 1.0;
    const double arg = 1.0 - ratio * ratio;
    return bessel_i0(beta * std::sqrt(std::max(0.0, arg))) / bessel_i0(beta);
}

// Kaiser beta from a stopband attenuation, per the usual design rule.
[[nodiscard]] double kaiser_beta_for(double attenuation_db)
{
    if (attenuation_db > 50.0) {
        return 0.1102 * (attenuation_db - 8.7);
    }
    if (attenuation_db >= 21.0) {
        return 0.5842 * std::pow(attenuation_db - 21.0, 0.4) + 0.07886 * (attenuation_db - 21.0);
    }
    return 0.0;
}

// Kaiser-windowed sinc lowpass. cutoff and transition are normalised to the
// sample rate, so 0.5 is Nyquist.
[[nodiscard]] std::vector<double> design_lowpass(double cutoff,
                                                 double transition,
                                                 double attenuation_db)
{
    const double beta = kaiser_beta_for(attenuation_db);
    const double estimate = (attenuation_db - 8.0) / (2.285 * kTwoPi * transition);
    auto length = static_cast<std::size_t>(std::ceil(std::max(estimate, 8.0)));
    if ((length % 2) == 0) {
        ++length;
    }

    std::vector<double> taps(length, 0.0);
    const double center = static_cast<double>(length - 1) / 2.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
        const double t = static_cast<double>(i) - center;
        const double sinc = (std::abs(t) < 1e-12)
                                ? 2.0 * cutoff
                                : std::sin(kTwoPi * cutoff * t) / (kPi * t);
        taps[i] = sinc * kaiser(i, length, beta);
        sum += taps[i];
    }
    for (double& tap : taps) {
        tap /= sum;
    }
    return taps;
}

[[nodiscard]] std::size_t next_power_of_two(std::size_t value)
{
    std::size_t result = 1;
    while (result < value) {
        result <<= 1U;
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// The clause 1.7 shaping filter
// ---------------------------------------------------------------------------

double shaping_impulse(double seconds, double bit_period_seconds)
{
    if (bit_period_seconds <= 0.0) {
        return 0.0;
    }
    const double u = seconds / bit_period_seconds;
    const double denominator = 1.0 - 64.0 * u * u;

    // Removable singularity at u = +/- 1/8: cos(4*pi/8) = 0 and the
    // denominator vanishes with it. The limit, by L'Hopital in u, is 2/td.
    if (std::abs(denominator) < 1e-9) {
        return 2.0 / bit_period_seconds;
    }
    return 8.0 * std::cos(4.0 * kPi * u) / (kPi * bit_period_seconds * denominator);
}

const char* lock_name(RdsLock state)
{
    switch (state) {
        case RdsLock::Unlocked:
            return "unlocked";
        case RdsLock::Acquiring:
            return "acquiring";
        case RdsLock::Locked:
            return "locked";
    }
    return "unlocked";
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Expected<RdsBitSync> RdsBitSync::create(const RdsBitsConfig& config)
{
    if (config.rate < kMinimumRateHz) {
        return fail(std::format(
            "RDS needs a composite rate of at least {} Hz and was given {}. The subcarrier "
            "reaches {} Hz, so anything below twice that aliases the data onto itself.",
            kMinimumRateHz, config.rate,
            kSubcarrierHz + kShapingCutoffHz));
    }
    if (config.lock_threshold <= config.unlock_threshold) {
        return fail(std::format(
            "RDS lock threshold {} must sit above the unlock threshold {}, or the decoder "
            "chatters between the two states on every bit.",
            config.lock_threshold, config.unlock_threshold));
    }
    if (config.lock_window_bits == 0 || config.acquisition_bits == 0) {
        return fail("RDS lock window and acquisition length must both be at least one bit");
    }
    if (config.carrier_loop_bandwidth_hz <= 0.0 || config.timing_loop_bandwidth_hz <= 0.0 ||
        config.carrier_acquisition_bandwidth_hz <= 0.0) {
        return fail("RDS loop bandwidths must be positive");
    }
    if (config.carrier_acquisition_bandwidth_hz < config.carrier_loop_bandwidth_hz) {
        return fail(std::format(
            "RDS carrier acquisition bandwidth {} Hz is narrower than the tracking bandwidth "
            "{} Hz, which inverts the gear shift: the loop would acquire slowly and then widen.",
            config.carrier_acquisition_bandwidth_hz, config.carrier_loop_bandwidth_hz));
    }

    RdsBitSync sync;
    sync.config_ = config;

    const auto rate = static_cast<double>(config.rate);

    sync.decimation_ = std::max(1, static_cast<int>(std::floor(rate / kTargetWorkingRateHz)));
    sync.working_rate_ = rate / static_cast<double>(sync.decimation_);
    sync.samples_per_bit_ = sync.working_rate_ / kBitRateHz;

    // Pilot loop, at the input rate. Phase is carried in turns rather than
    // radians because the subcarrier reference is three times it, and tripling
    // a value already reduced into [0, 1) is exact where tripling radians and
    // reducing modulo 2*pi is not.
    sync.pilot_increment_ = static_cast<double>(kPilotHz) / rate;
    const LoopGains pilot = loop_gains(20.0 / rate);
    sync.pilot_kp_ = pilot.proportional;
    sync.pilot_ki_ = pilot.integral;

    // The two measurement arms. Ten to one, so noise reads sqrt(0.1) = 0.32
    // through the pair and a tone reads one. Clause 1.1 puts the pilot within
    // +/- 2 Hz of 19 kHz, so even the narrow arm passes a real pilot before
    // the loop has corrected anything.
    sync.pilot_wide_alpha_ = 1.0 - std::exp(-kTwoPi * kPilotWideHz / rate);
    sync.pilot_narrow_alpha_ = 1.0 - std::exp(-kTwoPi * kPilotNarrowHz / rate);
    sync.pilot_magnitude_alpha_ = 1.0 - std::exp(-kTwoPi * 2.0 / rate);
    sync.composite_alpha_ = 1.0 - std::exp(-kTwoPi * 20.0 / rate);

    // Decimating anti-alias lowpass. Passband edge is the clause 1.7 cutoff;
    // stopband edge is where the working rate would fold content back into it.
    const double passband_hz = static_cast<double>(kShapingCutoffHz);
    const double stopband_hz = sync.working_rate_ - passband_hz;
    if (stopband_hz <= passband_hz * 1.5) {
        return fail(std::format(
            "RDS decimation left a working rate of {:.1f} Hz, which is too close to twice the "
            "{} Hz data bandwidth for an anti-alias filter to fit between them",
            sync.working_rate_, kShapingCutoffHz));
    }
    sync.decim_taps_ = design_lowpass((passband_hz + stopband_hz) / (2.0 * rate),
                                      (stopband_hz - passband_hz) / rate,
                                      kDecimatorStopbandDb);
    sync.decim_real_.assign(sync.decim_taps_.size(), 0.0);
    sync.decim_imag_.assign(sync.decim_taps_.size(), 0.0);

    // The clause 1.7 shaping filter, sampled at the working rate. taps[i] is
    // h(t)/rate: the continuous impulse response scaled by the sample period,
    // so the tap sum approaches HT(0) = 1 and the filter has unity DC gain.
    {
        const auto half =
            static_cast<std::size_t>(std::llround(static_cast<double>(kShapingSpanBits) *
                                                  sync.samples_per_bit_));
        const std::size_t length = 2 * half + 1;
        sync.shape_taps_.assign(length, 0.0);
        double sum = 0.0;
        for (std::size_t i = 0; i < length; ++i) {
            const double offset = static_cast<double>(i) - static_cast<double>(half);
            const double seconds = offset / sync.working_rate_;
            sync.shape_taps_[i] = shaping_impulse(seconds, kBitPeriodSeconds) *
                                  kaiser(i, length, kShapingKaiserBeta);
            sum += sync.shape_taps_[i];
        }
        for (double& tap : sync.shape_taps_) {
            tap /= sum;
        }
        sync.shape_real_.assign(length, 0.0);
        sync.shape_imag_.assign(length, 0.0);
    }

    const LoopGains track = loop_gains(config.carrier_loop_bandwidth_hz / sync.working_rate_);
    sync.carrier_kp_track_ = track.proportional;
    sync.carrier_ki_track_ = track.integral;
    const LoopGains acquire =
        loop_gains(config.carrier_acquisition_bandwidth_hz / sync.working_rate_);
    sync.carrier_kp_acquire_ = acquire.proportional;
    sync.carrier_ki_acquire_ = acquire.integral;

    // 5 Hz on the coherence estimator, which is about 600 working samples.
    // Coherence has to read near zero on noise for the timing gate below to
    // mean anything, and an estimate over N samples floors at 1/sqrt(N): at
    // 30 Hz that floor is 0.1 with excursions past 0.3, which is the same
    // order as the gate itself.
    sync.coherence_alpha_ = 1.0 - std::exp(-kTwoPi * 5.0 / sync.working_rate_);

    const LoopGains timing = loop_gains(config.timing_loop_bandwidth_hz / kBitRateHz);
    sync.timing_kp_ = timing.proportional;
    sync.timing_ki_ = timing.integral;
    sync.bit_period_ = sync.samples_per_bit_;

    // History has to reach back one bit for the previous decision and forward
    // through the late half-bit sample, plus the cubic interpolator's own two
    // taps either side. Four bit periods, rounded up to a power of two so the
    // index wrap is a mask.
    sync.history_.assign(next_power_of_two(
                             static_cast<std::size_t>(std::ceil(4.0 * sync.samples_per_bit_)) + 8),
                         0.0);
    sync.history_mask_ = sync.history_.size() - 1;

    sync.scan_accumulator_.assign(kScanPhases, 0.0);
    sync.consistency_window_.assign(config.lock_window_bits, 0);

    sync.reset();
    return sync;
}

void RdsBitSync::reset()
{
    status_ = RdsBitsStatus{};

    pilot_phase_ = 0.0;
    pilot_freq_ = 0.0;
    pilot_wide_real_[0] = pilot_wide_real_[1] = 0.0;
    pilot_wide_imag_[0] = pilot_wide_imag_[1] = 0.0;
    pilot_narrow_real_[0] = pilot_narrow_real_[1] = 0.0;
    pilot_narrow_imag_[0] = pilot_narrow_imag_[1] = 0.0;
    pilot_wide_magnitude_ = 0.0;
    pilot_narrow_magnitude_ = 0.0;
    composite_power_ = 0.0;

    std::ranges::fill(decim_real_, 0.0);
    std::ranges::fill(decim_imag_, 0.0);
    decim_write_ = 0;
    decim_count_ = 0;

    std::ranges::fill(shape_real_, 0.0);
    std::ranges::fill(shape_imag_, 0.0);
    shape_write_ = 0;

    // Not zero. A BPSK phase detector has unstable equilibria a quarter turn
    // away from its stable ones, and clause 1.2 explicitly permits a
    // transmitter to lock the subcarrier in QUADRATURE with the third
    // harmonic of the pilot. Starting at exactly zero against exactly that
    // transmitter puts the loop on the unstable point with zero error, where
    // it sits until noise pushes it off. A twentieth of a radian costs one
    // loop time constant and removes the case.
    carrier_phase_ = 0.05;
    carrier_freq_ = 0.0;
    coherence_real_ = 0.0;
    coherence_imag_ = 0.0;
    coherence_power_ = 0.0;

    std::ranges::fill(history_, 0.0);
    history_count_ = 0;

    bit_position_ = 0.0;
    bit_period_ = samples_per_bit_;

    std::ranges::fill(scan_accumulator_, 0.0);
    scan_bits_ = 0;
    scanning_ = true;

    std::ranges::fill(consistency_window_, static_cast<std::uint8_t>(0));
    consistency_write_ = 0;
    consistency_filled_ = 0;
    consistency_hits_ = 0;
    unlock_run_ = 0;

    have_previous_ = false;
    previous_bit_ = false;

    queue_.clear();
}

void RdsBitSync::begin_reacquisition()
{
    status_.lock = RdsLock::Unlocked;

    scanning_ = true;
    scan_bits_ = 0;
    std::ranges::fill(scan_accumulator_, 0.0);

    // The window goes with the scan. A consistency figure measured against
    // the old timing phase says nothing about the new one, and leaving it in
    // place is what let a relock certify itself on evidence it gathered
    // before the signal went away.
    std::ranges::fill(consistency_window_, static_cast<std::uint8_t>(0));
    consistency_write_ = 0;
    consistency_filled_ = 0;
    consistency_hits_ = 0;
    unlock_run_ = 0;

    // Reported as the coin toss an empty window is, rather than left holding
    // the last figure the old lock produced. A caller watching quality has to
    // see the decoder lose confidence, not read 0.99 through a dropout.
    status_.biphase_consistency = 0.5;
    status_.quality = 0.0;

    // Clause 1.6 decodes each bit against its predecessor, and the bit before
    // a dropout is not the predecessor of the bit after it.
    have_previous_ = false;
}

// ---------------------------------------------------------------------------
// Input rate: pilot, mixer, decimator
// ---------------------------------------------------------------------------

void RdsBitSync::push_input(float sample, const BitSink* sink)
{
    const double x = static_cast<double>(sample);

    composite_power_ += composite_alpha_ * (x * x - composite_power_);
    const double composite_rms = std::sqrt(std::max(composite_power_, 1e-30));

    // Pilot loop. The phase is advanced first and corrected afterwards so that
    // the subcarrier reference this sample uses and the one the phase detector
    // measured against are the same phase.
    if (config_.track_pilot) {
        const double angle = kTwoPi * pilot_phase_;
        const double c = std::cos(angle);
        const double s = std::sin(angle);

        // x * exp(-j*angle). The real input puts an image at twice the pilot
        // frequency, which the two one-poles below remove.
        const double mixed_real = x * c;
        const double mixed_imag = -x * s;

        pilot_wide_real_[0] += pilot_wide_alpha_ * (mixed_real - pilot_wide_real_[0]);
        pilot_wide_imag_[0] += pilot_wide_alpha_ * (mixed_imag - pilot_wide_imag_[0]);
        pilot_wide_real_[1] += pilot_wide_alpha_ * (pilot_wide_real_[0] - pilot_wide_real_[1]);
        pilot_wide_imag_[1] += pilot_wide_alpha_ * (pilot_wide_imag_[0] - pilot_wide_imag_[1]);

        pilot_narrow_real_[0] += pilot_narrow_alpha_ * (mixed_real - pilot_narrow_real_[0]);
        pilot_narrow_imag_[0] += pilot_narrow_alpha_ * (mixed_imag - pilot_narrow_imag_[0]);
        pilot_narrow_real_[1] +=
            pilot_narrow_alpha_ * (pilot_narrow_real_[0] - pilot_narrow_real_[1]);
        pilot_narrow_imag_[1] +=
            pilot_narrow_alpha_ * (pilot_narrow_imag_[0] - pilot_narrow_imag_[1]);

        const double wide = std::hypot(pilot_wide_real_[1], pilot_wide_imag_[1]);
        const double narrow = std::hypot(pilot_narrow_real_[1], pilot_narrow_imag_[1]);
        pilot_wide_magnitude_ += pilot_magnitude_alpha_ * (wide - pilot_wide_magnitude_);
        pilot_narrow_magnitude_ += pilot_magnitude_alpha_ * (narrow - pilot_narrow_magnitude_);

        // Times two because mixing a real signal down splits its energy
        // between the wanted image and the one at twice the carrier, so the
        // surviving phasor is half the tone's amplitude.
        status_.pilot_level = 2.0 * pilot_narrow_magnitude_ / composite_rms;
        status_.pilot_tone_ratio =
            (pilot_wide_magnitude_ > 1e-30)
                ? std::min(1.0, pilot_narrow_magnitude_ / pilot_wide_magnitude_)
                : 0.0;
        status_.pilot_locked =
            status_.pilot_tone_ratio >= config_.pilot_tone_ratio_threshold &&
            status_.pilot_level >= config_.pilot_minimum_level;

        if (status_.pilot_locked) {
            // Phase error in turns, from the wide arm: the loop bandwidth is
            // 20 Hz, so a 250 Hz prefilter is already a decade narrower than
            // it needs and the narrow arm would add a pole inside the loop.
            //
            // imag/magnitude is the small-angle reading of the argument. It
            // saturates gracefully where atan2 would wrap, which is exactly
            // the moment the loop is furthest from lock.
            const double error = (pilot_wide_imag_[1] / std::max(wide, 1e-30)) / kTwoPi;
            pilot_freq_ += pilot_ki_ * error;
            pilot_phase_ += pilot_kp_ * error;
        } else {
            // Frozen at nominal rather than left to random-walk on noise. The
            // walk would be tripled on the way to the subcarrier, and the
            // carrier loop below would then be chasing this loop rather than
            // the transmitter.
            pilot_freq_ = 0.0;
        }
    }

    pilot_phase_ += pilot_increment_ + pilot_freq_;
    pilot_phase_ -= std::floor(pilot_phase_);

    // EN 50067:1998 clause 1.1: the subcarrier is the third harmonic of the
    // pilot. Tripling a phase already reduced into [0, 1) is exact, which is
    // why the pilot phase is carried in turns.
    double sub_turns = 3.0 * pilot_phase_;
    sub_turns -= std::floor(sub_turns);
    const double sub_angle = kTwoPi * sub_turns;

    const double mix_real = x * std::cos(sub_angle);
    const double mix_imag = -x * std::sin(sub_angle);

    decim_real_[decim_write_] = mix_real;
    decim_imag_[decim_write_] = mix_imag;
    decim_write_ = (decim_write_ + 1 == decim_real_.size()) ? 0 : decim_write_ + 1;

    ++status_.samples_consumed;
    if (++decim_count_ < decimation_) {
        return;
    }
    decim_count_ = 0;

    double sum_real = 0.0;
    double sum_imag = 0.0;
    std::size_t index = decim_write_;
    for (std::size_t i = decim_taps_.size(); i-- > 0;) {
        sum_real += decim_taps_[i] * decim_real_[index];
        sum_imag += decim_taps_[i] * decim_imag_[index];
        index = (index + 1 == decim_real_.size()) ? 0 : index + 1;
    }
    push_working(sum_real, sum_imag, sink);
}

// ---------------------------------------------------------------------------
// Working rate: shaping, carrier recovery, timing
// ---------------------------------------------------------------------------

void RdsBitSync::push_working(double real_part, double imag_part, const BitSink* sink)
{
    shape_real_[shape_write_] = real_part;
    shape_imag_[shape_write_] = imag_part;
    shape_write_ = (shape_write_ + 1 == shape_real_.size()) ? 0 : shape_write_ + 1;

    double shaped_real = 0.0;
    double shaped_imag = 0.0;
    {
        std::size_t index = shape_write_;
        for (std::size_t i = shape_taps_.size(); i-- > 0;) {
            shaped_real += shape_taps_[i] * shape_real_[index];
            shaped_imag += shape_taps_[i] * shape_imag_[index];
            index = (index + 1 == shape_real_.size()) ? 0 : index + 1;
        }
    }

    // Derotate by the carrier loop's estimate.
    const double c = std::cos(carrier_phase_);
    const double s = std::sin(carrier_phase_);
    const double zr = shaped_real * c + shaped_imag * s;
    const double zi = -shaped_real * s + shaped_imag * c;

    const double power = zr * zr + zi * zi;

    // BPSK phase detector. real*imag/|z|^2 is sin(2*phi)/2, which reads as the
    // phase error for small errors and stays bounded for large ones. Its two
    // stable points are 180 degrees apart, which is the ambiguity clause 1.6's
    // differential coding exists to absorb, and it is indifferent to whether
    // the transmitter locked the subcarrier in phase or in quadrature with the
    // third harmonic of the pilot, which clause 1.2 leaves as the
    // transmitter's choice.
    const double error = (power > 1e-30) ? (zr * zi / power) : 0.0;
    const bool tracking = status_.lock == RdsLock::Locked;
    const double kp = tracking ? carrier_kp_track_ : carrier_kp_acquire_;
    const double ki = tracking ? carrier_ki_track_ : carrier_ki_acquire_;
    carrier_freq_ += ki * error;
    carrier_phase_ += carrier_freq_ + kp * error;
    if (carrier_phase_ > kPi) {
        carrier_phase_ -= kTwoPi;
    } else if (carrier_phase_ < -kPi) {
        carrier_phase_ += kTwoPi;
    }
    status_.carrier_offset_hz = carrier_freq_ * working_rate_ / kTwoPi;

    // Coherence: |E[z^2]| / E[|z|^2]. For a BPSK signal in circular Gaussian
    // noise this is exactly SNR/(1+SNR) in the post-filter bandwidth, because
    // the noise contributes nothing to E[z^2] and everything to E[|z|^2].
    coherence_real_ += coherence_alpha_ * ((zr * zr - zi * zi) - coherence_real_);
    coherence_imag_ += coherence_alpha_ * ((2.0 * zr * zi) - coherence_imag_);
    coherence_power_ += coherence_alpha_ * (power - coherence_power_);
    status_.carrier_coherence =
        (coherence_power_ > 1e-30)
            ? std::min(1.0, std::hypot(coherence_real_, coherence_imag_) / coherence_power_)
            : 0.0;

    history_[history_count_ & history_mask_] = zr;
    ++history_count_;

    run_timing(sink);
}

double RdsBitSync::interpolate(double position) const
{
    // Catmull-Rom on four neighbours. The signal is band-limited to 2375 Hz
    // and sampled at sixteen times the bit rate, so it is oversampled about
    // four times against its own Nyquist rate and a cubic is well inside the
    // noise.
    const double floor_position = std::floor(position);
    const auto base = static_cast<std::int64_t>(floor_position);
    const double mu = position - floor_position;

    const auto at = [this](std::int64_t index) {
        return history_[static_cast<std::uint64_t>(index) & history_mask_];
    };
    const double p0 = at(base - 1);
    const double p1 = at(base);
    const double p2 = at(base + 1);
    const double p3 = at(base + 2);

    const double a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
    const double b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
    const double d = -0.5 * p0 + 0.5 * p2;
    return ((a * mu + b) * mu + d) * mu + p1;
}

double RdsBitSync::biphase_statistic(double position) const
{
    // EN 50067:1998 clause 1.7: logic 1 is the impulse pair
    // delta(t) - delta(t - td/2) and logic 0 is its negative. Correlating
    // against that pair is one subtraction of the shaped waveform at the two
    // half-bit instants, and it contributes no shaping of its own, which is
    // what makes the clause 1.7 filter in front of it the right matched
    // filter rather than an over-filter.
    return interpolate(position) - interpolate(position + 0.5 * bit_period_);
}

void RdsBitSync::run_timing(const BitSink* sink)
{
    for (;;) {
        // The interpolator reaches two samples past the position it is asked
        // for. While tracking, the furthest position wanted is the second
        // half-bit sample; while scanning, it is the last candidate phase plus
        // that same half bit.
        const double needed = (scanning_ ? 1.5 : 0.5) * bit_period_ + 3.0;
        if (static_cast<double>(history_count_) < bit_position_ + needed) {
            return;
        }

        if (bit_position_ < static_cast<double>(history_count_) - 3.0 * bit_period_) {
            // The loop fell so far behind that the history has rolled over it.
            // Only reachable if bit_period_ collapsed, which the clamp below
            // prevents, but a silent wrap here would read stale samples as
            // signal.
            bit_position_ = static_cast<double>(history_count_) - needed;
        }

        if (status_.carrier_coherence < config_.carrier_lock_threshold) {
            // Do not attempt timing while the data axis is still turning. A
            // scan run against a rotating constellation picks a phase at
            // random and the tracking loop then holds it, which looks exactly
            // like a lock that will not improve.
            //
            // Counted only on the way in. This branch runs once per bit
            // period for the whole length of a fade, and a counter that
            // climbed by a thousand on one dropout would say nothing a caller
            // could read.
            if (!scanning_) {
                ++status_.reacquisitions;
            }
            begin_reacquisition();
            bit_position_ += bit_period_;
            continue;
        }

        if (scanning_) {
            // ACQUISITION, and why it is a scan rather than a wider loop.
            //
            // A biphase waveform gives the timing detector two stable points
            // per bit, not one: sampling half a bit early pairs the second
            // half of one bit with the first half of the next, which is a
            // perfectly good looking eye. The decision statistic tells them
            // apart, because at the true instant it is the full 2A and at the
            // half-bit point it is A times the sum of two adjacent symbols,
            // which averages to half that. A scan over one bit period sees the
            // two-to-one margin directly. A tracking loop cannot: both points
            // are stable for it, and it holds whichever it fell into.
            for (std::size_t phase = 0; phase < kScanPhases; ++phase) {
                const double offset = bit_period_ * static_cast<double>(phase) /
                                      static_cast<double>(kScanPhases);
                scan_accumulator_[phase] += std::abs(biphase_statistic(bit_position_ + offset));
            }
            ++scan_bits_;
            bit_position_ += bit_period_;

            if (scan_bits_ >= config_.acquisition_bits) {
                const auto best = std::ranges::max_element(scan_accumulator_);
                const auto phase = static_cast<std::size_t>(
                    std::distance(scan_accumulator_.begin(), best));
                bit_position_ += bit_period_ * static_cast<double>(phase) /
                                 static_cast<double>(kScanPhases);
                scanning_ = false;
                scan_bits_ = 0;
                std::ranges::fill(scan_accumulator_, 0.0);
                status_.lock = RdsLock::Acquiring;
            }
            continue;
        }

        const double first = interpolate(bit_position_);
        const double second = interpolate(bit_position_ + 0.5 * bit_period_);
        const double statistic = first - second;

        // Timing error from the mid-bit zero crossing.
        //
        // Biphase guarantees a transition at the middle of every bit whatever
        // the data does, which a bit-boundary detector cannot say: consecutive
        // equal symbols leave the boundary flat. The waveform crosses zero a
        // quarter of a bit after the decision instant, and its value there is
        // proportional to how far off the instant is, signed by the symbol.
        const double crossing = interpolate(bit_position_ + 0.25 * bit_period_);
        const double magnitude = 0.5 * std::abs(statistic);
        double error = 0.0;
        if (magnitude > 1e-30) {
            const double sign = (statistic >= 0.0) ? 1.0 : -1.0;
            error = (crossing * sign / magnitude) * bit_period_ / kZeroCrossingGain;
            // One eighth of a bit per correction. An outlier from a deep noise
            // excursion would otherwise throw the loop out of lock in a single
            // step, and the loop's whole job is to average over many bits.
            error = std::clamp(error, -0.125 * bit_period_, 0.125 * bit_period_);
        }

        bit_period_ += timing_ki_ * error;
        bit_period_ = std::clamp(bit_period_, 0.9 * samples_per_bit_, 1.1 * samples_per_bit_);
        bit_position_ += bit_period_ + timing_kp_ * error;
        status_.bit_rate_hz = working_rate_ / bit_period_;

        // Biphase sign consistency. Clause 1.7's impulse pair is odd, so the
        // two halves of a bit always have opposite signs. Data cannot change
        // that, which is why this reads as a channel measurement rather than a
        // payload statistic.
        const auto consistent =
            static_cast<std::uint8_t>(((first >= 0.0) != (second >= 0.0)) ? 1 : 0);
        if (consistency_filled_ == consistency_window_.size()) {
            consistency_hits_ -= consistency_window_[consistency_write_];
        } else {
            ++consistency_filled_;
        }
        consistency_hits_ += consistent;
        consistency_window_[consistency_write_] = consistent;
        consistency_write_ =
            (consistency_write_ + 1 == consistency_window_.size()) ? 0 : consistency_write_ + 1;

        status_.biphase_consistency =
            static_cast<double>(consistency_hits_) / static_cast<double>(consistency_filled_);
        status_.quality = std::clamp(2.0 * (status_.biphase_consistency - 0.5), 0.0, 1.0);

        const bool window_full = consistency_filled_ == consistency_window_.size();
        if (window_full && status_.biphase_consistency >= config_.lock_threshold) {
            status_.lock = RdsLock::Locked;
            unlock_run_ = 0;
        } else if (window_full && status_.biphase_consistency < config_.unlock_threshold) {
            status_.lock = RdsLock::Acquiring;
            have_previous_ = false;
            // Give up and re-scan rather than sit in a bad lock. A whole
            // window below the unlock threshold means the phase is wrong, not
            // that the signal is briefly weak.
            if (++unlock_run_ >= consistency_window_.size()) {
                ++status_.reacquisitions;
                begin_reacquisition();
            }
        } else if (status_.lock != RdsLock::Locked) {
            status_.lock = RdsLock::Acquiring;
        }

        if (status_.lock != RdsLock::Locked) {
            have_previous_ = false;
            continue;
        }

        // EN 50067:1998 clause 1.7: logic 1 puts the positive impulse first.
        // A global inversion here decodes identically once clause 1.6 has run,
        // which is the entire reason clause 1.6 exists, so this polarity is a
        // choice the differential decoder makes irrelevant rather than a
        // decision that has to be right.
        const bool received = statistic >= 0.0;

        // EN 50067:1998 clause 1.6, Table 2: the decoded output is the
        // previous input XOR the new input. Rows (0,0)->0, (0,1)->1, (1,0)->1,
        // (1,1)->0.
        if (have_previous_) {
            emit(received != previous_bit_, sink);
        }
        previous_bit_ = received;
        have_previous_ = true;
    }
}

void RdsBitSync::emit(bool bit, const BitSink* sink)
{
    ++status_.bits_emitted;
    if (sink != nullptr) {
        (*sink)(bit);
    } else {
        queue_.push_back(static_cast<std::uint8_t>(bit ? 1 : 0));
    }
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void RdsBitSync::process(ConstRealSpan mpx, const BitSink& sink)
{
    for (const float sample : mpx) {
        push_input(sample, &sink);
    }
}

void RdsBitSync::process(ConstRealSpan mpx)
{
    for (const float sample : mpx) {
        push_input(sample, nullptr);
    }
}

std::vector<std::uint8_t> RdsBitSync::drain()
{
    std::vector<std::uint8_t> out;
    out.swap(queue_);
    return out;
}

}  // namespace revenant::decode
