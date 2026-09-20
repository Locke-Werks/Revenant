#include "core/dsp/synth/rds_mod.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <random>

namespace revenant::siggen {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;

// Truncation of the clause 1.7 impulse response, in bit periods either side.
// It falls as 1/t^2, so eight bit periods is about 72 dB down. The receiver
// uses five; the transmitter can afford more and there is no reason to put
// avoidable error into the reference signal a decoder is scored against.
constexpr std::size_t kShapingSpanBits = 8;

// Table resolution of that impulse response, in steps per bit period. The
// response varies on the scale of a quarter of a bit, so 512 steps puts the
// linear interpolation residue around 100 dB down: below the shaping
// truncation, and far below any noise floor this is used at.
constexpr double kShapingStepsPerBit = 512.0;

// Seed domains, mixed by derive_seed the same way channel.cpp's are. The
// values only have to be distinct from each other.
constexpr std::uint64_t kDomainRealAwgn = 0x5244'5300;  // "RDS\0"

// Uniform in (0, 1] from the top 53 bits, and Box-Muller on top of it.
//
// The same construction as core/dsp/synth/channel.cpp, and for the same
// reason: the standard does not specify how std::uniform_real_distribution or
// std::normal_distribution consume their engine, so the same seed gives
// different numbers under different standard libraries and a printed seed
// stops reproducing a failure. The helpers there are file-local, which is why
// this is not a call into them.
[[nodiscard]] double next_uniform(std::mt19937_64& engine)
{
    constexpr double kInverse2Pow53 = 1.0 / 9007199254740992.0;
    const std::uint64_t bits = engine() >> 11;
    return (static_cast<double>(bits) + 1.0) * kInverse2Pow53;
}

struct GaussianPair {
    double first = 0.0;
    double second = 0.0;
};

[[nodiscard]] GaussianPair next_gaussian_pair(std::mt19937_64& engine)
{
    const double uniform_radius = next_uniform(engine);
    const double uniform_angle = next_uniform(engine);
    const double radius = std::sqrt(-2.0 * std::log(uniform_radius));
    const double angle = kTwoPi * uniform_angle;
    return GaussianPair{radius * std::cos(angle), radius * std::sin(angle)};
}

[[nodiscard]] double turns_to_sine(double turns)
{
    return std::sin(kTwoPi * (turns - std::floor(turns)));
}

}  // namespace

double real_mean_power(dsp::ConstRealSpan samples)
{
    if (samples.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const float value : samples) {
        const double v = static_cast<double>(value);
        sum += v * v;
    }
    return sum / static_cast<double>(samples.size());
}

double real_peak(dsp::ConstRealSpan samples)
{
    double peak = 0.0;
    for (const float value : samples) {
        peak = std::max(peak, std::abs(static_cast<double>(value)));
    }
    return peak;
}

// ---------------------------------------------------------------------------
// Validation and sizing
// ---------------------------------------------------------------------------

Status validate(const RdsModSpec& spec)
{
    if (spec.rate < decode::kMinimumRateHz) {
        return fail(std::format(
            "RDS composite rate {} is below {} Hz. The subcarrier reaches {} Hz, so a lower "
            "rate aliases the data onto itself before anything can decode it.",
            spec.rate, decode::kMinimumRateHz,
            decode::kSubcarrierHz + decode::kShapingCutoffHz));
    }
    if (spec.bits.empty()) {
        return fail("RDS modulator needs at least one bit to transmit");
    }
    for (const std::uint8_t bit : spec.bits) {
        if (bit > 1) {
            return fail(std::format(
                "RDS payload carries the value {}. One bit per element, 0 or 1: the payload is "
                "unpacked on purpose so a packing disagreement cannot hide in it.",
                bit));
        }
    }
    if (spec.rds_deviation_hz <= 0) {
        return fail("RDS subcarrier deviation must be positive");
    }
    if (spec.pilot_enabled && spec.pilot_deviation_hz <= 0) {
        return fail("the pilot is enabled but its deviation is not positive");
    }
    if (spec.mono_deviation_hz < 0) {
        return fail("mono tone deviation cannot be negative");
    }
    if (std::abs(spec.clock_error_ppm) > 1000.0) {
        return fail(std::format(
            "clock error {} ppm is beyond anything a transmitter does. EN 50067 clause 1.5 "
            "allows +/- 0.125 bit/s on 1187.5, which is 105 ppm.",
            spec.clock_error_ppm));
    }
    return {};
}

Expected<std::size_t> rds_nominal_sample_count(const RdsModSpec& spec)
{
    Status checked = validate(spec);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    const double scale = 1.0 + spec.clock_error_ppm * 1e-6;
    const double bit_period = 1.0 / (decode::kBitRateHz * scale);
    const double span = static_cast<double>(kShapingSpanBits);
    const double seconds =
        (static_cast<double>(spec.bits.size() - 1) + 2.0 * span + 0.5) * bit_period;
    return static_cast<std::size_t>(std::ceil(seconds * static_cast<double>(spec.rate))) + 2;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Expected<RdsModulator> RdsModulator::create(RdsModSpec spec)
{
    Status checked = validate(spec);
    if (!checked) {
        return std::unexpected(checked.error());
    }

    RdsModulator mod;
    mod.spec_ = std::move(spec);

    const auto rate = static_cast<double>(mod.spec_.rate);
    mod.clock_scale_ = 1.0 + mod.spec_.clock_error_ppm * 1e-6;
    mod.bit_rate_ = decode::kBitRateHz * mod.clock_scale_;
    mod.bit_period_seconds_ = 1.0 / mod.bit_rate_;
    mod.samples_per_bit_ = rate / mod.bit_rate_;

    // EN 50067:1998 clause 1.6 Table 1: the new output bit is the previous
    // output bit XOR the new input bit. Input 0 leaves the output unchanged,
    // input 1 complements it.
    mod.encoded_.resize(mod.spec_.bits.size());
    bool state = mod.spec_.initial_differential_state;
    for (std::size_t i = 0; i < mod.spec_.bits.size(); ++i) {
        if (mod.spec_.differential_encode) {
            state = state != (mod.spec_.bits[i] != 0);
        } else {
            state = mod.spec_.bits[i] != 0;
        }
        mod.encoded_[i] = static_cast<std::uint8_t>(state ? 1 : 0);
    }

    // EN 50067:1998 clause 1.1: the subcarrier is the third harmonic of the
    // pilot, so only the pilot's rate is carried and the subcarrier is three
    // times it. subcarrier_offset_hz rides on top and is not coherent with
    // anything, which is what it is for.
    mod.pilot_turns_per_sample_ =
        static_cast<double>(decode::kPilotHz) * mod.clock_scale_ / rate;
    mod.subcarrier_extra_turns_per_sample_ =
        static_cast<double>(mod.spec_.subcarrier_offset_hz) / rate;
    mod.mono_turns_per_sample_ =
        static_cast<double>(mod.spec_.mono_tone_hz) * mod.clock_scale_ / rate;

    const auto peak_deviation = static_cast<double>(kCompositePeakDeviationHz);
    mod.pilot_amplitude_ = mod.spec_.pilot_enabled
                               ? static_cast<double>(mod.spec_.pilot_deviation_hz) / peak_deviation
                               : 0.0;
    mod.mono_amplitude_ = static_cast<double>(mod.spec_.mono_deviation_hz) / peak_deviation;

    // Tabulate the clause 1.7 impulse response. shaping_impulse() is the
    // decoder's, because the clause splits one filter between the two ends.
    mod.shaping_span_bits_ = static_cast<double>(kShapingSpanBits);
    mod.shaping_steps_per_bit_ = kShapingStepsPerBit;
    {
        const auto half = static_cast<std::size_t>(kShapingSpanBits * kShapingStepsPerBit);
        mod.shaping_table_.assign(2 * half + 1, 0.0);
        for (std::size_t i = 0; i < mod.shaping_table_.size(); ++i) {
            const double bit_periods =
                (static_cast<double>(i) - static_cast<double>(half)) / kShapingStepsPerBit;
            mod.shaping_table_[i] = decode::shaping_impulse(bit_periods * mod.bit_period_seconds_,
                                                            mod.bit_period_seconds_);
        }
        // Normalise the peak to one so the injection scaling below starts
        // from a known place. The absolute scale of the impulse response is
        // 8/(pi*td), which is a per-rate number of no interest here: what
        // clause 1.3 fixes is the deviation of the finished signal.
        double peak = 0.0;
        for (const double value : mod.shaping_table_) {
            peak = std::max(peak, std::abs(value));
        }
        if (peak > 0.0) {
            for (double& value : mod.shaping_table_) {
                value /= peak;
            }
        }
    }

    Expected<std::size_t> count = rds_nominal_sample_count(mod.spec_);
    if (!count) {
        return std::unexpected(count.error());
    }
    mod.nominal_samples_ = *count;

    // Scale the data signal so its peak is the deviation clause 1.3 asks for.
    //
    // Measured once here over the whole bit sequence rather than at render
    // time, which is what keeps render() pure and blocking independent: the
    // scale is a property of the spec, not of how a caller chose to slice the
    // output. The bit sequence is finite and given, so the measurement is
    // well defined.
    mod.rds_scale_ = 1.0;
    double data_peak = 0.0;
    for (std::size_t i = 0; i < mod.nominal_samples_; ++i) {
        data_peak = std::max(data_peak, std::abs(mod.data_signal(i)));
    }
    if (data_peak <= 0.0) {
        return fail("the RDS data signal came out identically zero, which no bit sequence does");
    }
    mod.envelope_peak_ = static_cast<double>(mod.spec_.rds_deviation_hz) / peak_deviation;
    mod.rds_scale_ = mod.envelope_peak_ / data_peak;

    return mod;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

double RdsModulator::shaping_at(double bit_periods) const
{
    if (std::abs(bit_periods) >= shaping_span_bits_) {
        return 0.0;
    }
    const double position =
        (bit_periods + shaping_span_bits_) * shaping_steps_per_bit_;
    const double floor_position = std::floor(position);
    const auto index = static_cast<std::size_t>(floor_position);
    if (index + 1 >= shaping_table_.size()) {
        return shaping_table_.back();
    }
    const double mu = position - floor_position;
    return shaping_table_[index] * (1.0 - mu) + shaping_table_[index + 1] * mu;
}

double RdsModulator::bit_instant_samples(std::size_t bit) const
{
    // Bit i sits at (i + span) bit periods, so bit zero's leading shaping tail
    // starts at sample zero rather than being cut off by the start of the
    // buffer.
    return (static_cast<double>(bit) + shaping_span_bits_) * samples_per_bit_ -
           spec_.start_offset_samples;
}

double RdsModulator::data_signal(SampleIndex index) const
{
    // Time in bit periods from the start of the buffer, with bit zero placed
    // at shaping_span_bits_.
    const double position =
        (static_cast<double>(index) + spec_.start_offset_samples) / samples_per_bit_ -
        shaping_span_bits_;

    // EN 50067:1998 clause 1.7: logic 1 is the impulse pair
    // delta(t) - delta(t - td/2), logic 0 is its negative. A bit contributes
    // through both impulses, so the range of bits in reach is bounded by the
    // later one.
    const double low = position - shaping_span_bits_;
    const double high = position + shaping_span_bits_;
    const auto first =
        static_cast<std::int64_t>(std::ceil(low - 0.5));
    const auto last = static_cast<std::int64_t>(std::floor(high));

    const auto count = static_cast<std::int64_t>(encoded_.size());
    const std::int64_t begin = std::max<std::int64_t>(0, first);
    const std::int64_t end = std::min<std::int64_t>(count - 1, last);

    double sum = 0.0;
    for (std::int64_t i = begin; i <= end; ++i) {
        const double offset = position - static_cast<double>(i);
        const double symbol = (encoded_[static_cast<std::size_t>(i)] != 0) ? 1.0 : -1.0;
        sum += symbol * (shaping_at(offset) - shaping_at(offset - 0.5));
    }
    return sum * rds_scale_;
}

void RdsModulator::render_rds_only(SampleIndex start, dsp::RealSpan out) const
{
    for (std::size_t i = 0; i < out.size(); ++i) {
        const SampleIndex index = start + i;
        const double t = static_cast<double>(index) + spec_.start_offset_samples;
        const double pilot_turns = pilot_turns_per_sample_ * t;
        const double sub_turns = 3.0 * pilot_turns + subcarrier_extra_turns_per_sample_ * t +
                                 spec_.subcarrier_phase_radians / kTwoPi;

        // EN 50067:1998 clause 1.4: suppressed-carrier amplitude modulation of
        // the subcarrier by the shaped biphase signal.
        out[i] = static_cast<float>(data_signal(index) * turns_to_sine(sub_turns));
    }
}

void RdsModulator::render(SampleIndex start, dsp::RealSpan out) const
{
    for (std::size_t i = 0; i < out.size(); ++i) {
        const SampleIndex index = start + i;
        const double t = static_cast<double>(index) + spec_.start_offset_samples;

        const double pilot_turns = pilot_turns_per_sample_ * t;
        const double sub_turns = 3.0 * pilot_turns + subcarrier_extra_turns_per_sample_ * t +
                                 spec_.subcarrier_phase_radians / kTwoPi;

        double value = data_signal(index) * turns_to_sine(sub_turns);

        // The pilot and the subcarrier reference are both sines of the same
        // phase variable, so subcarrier_phase_radians of zero is literally
        // "in phase with the third harmonic of the pilot" as clause 1.2 puts
        // it, and pi/2 is literally "in quadrature".
        if (pilot_amplitude_ > 0.0) {
            value += pilot_amplitude_ * turns_to_sine(pilot_turns);
        }
        if (mono_amplitude_ > 0.0) {
            value += mono_amplitude_ * turns_to_sine(mono_turns_per_sample_ * t);
        }
        out[i] = static_cast<float>(value);
    }
}

// ---------------------------------------------------------------------------
// One shot
// ---------------------------------------------------------------------------

double RdsComposite::bit_instant_samples(std::size_t bit) const
{
    return first_bit_instant_samples + static_cast<double>(bit) * samples_per_bit;
}

Expected<RdsComposite> generate_rds(const RdsModSpec& spec, std::size_t sample_count)
{
    Expected<RdsModulator> mod = RdsModulator::create(spec);
    if (!mod) {
        return std::unexpected(mod.error());
    }

    const std::size_t count = (sample_count > 0) ? sample_count : mod->nominal_sample_count();

    RdsComposite out;
    out.samples.assign(count, 0.0F);
    mod->render(0, dsp::RealSpan(out.samples));

    std::vector<float> rds_only(count, 0.0F);
    mod->render_rds_only(0, dsp::RealSpan(rds_only));

    out.data_bits = spec.bits;
    out.transmitted_bits = mod->transmitted_bits();
    out.rds_peak = real_peak(rds_only);
    out.rds_envelope_peak = mod->envelope_peak();
    out.rds_mean_power = real_mean_power(rds_only);
    out.rds_rms = std::sqrt(out.rds_mean_power);
    out.composite_mean_power = real_mean_power(out.samples);
    out.composite_peak = real_peak(out.samples);
    out.bit_rate_hz = mod->bit_rate_hz();
    out.samples_per_bit = mod->samples_per_bit();
    out.first_bit_instant_samples = mod->bit_instant_samples(0);
    return out;
}

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

Expected<double> real_awgn_power_for(const NoiseLevel& level,
                                     double signal_power,
                                     SampleRate rate)
{
    if (rate <= 1) {
        return fail("a real composite needs a positive sample rate to calibrate noise against");
    }
    if (!(signal_power > 0.0)) {
        return fail("cannot calibrate noise against a signal of zero power");
    }

    // A real stream at `rate` represents rate/2 hertz of spectrum, so every
    // conversion in channel.h applies with the rate halved. The integer
    // division loses at most half a hertz out of sixty thousand on an odd
    // rate, which is 3e-5 dB, and keeping the whole calculation on the
    // existing SampleRate type is worth more than that.
    Expected<double> full_band_db = level.to_full_sample_rate_db(rate / 2);
    if (!full_band_db) {
        return std::unexpected(full_band_db.error());
    }
    return signal_power / std::pow(10.0, *full_band_db / 10.0);
}

Expected<RdsNoiseReport> add_real_awgn(dsp::RealSpan composite,
                                       double signal_power,
                                       const NoiseLevel& level,
                                       SampleRate rate,
                                       std::uint64_t seed)
{
    Expected<double> power = real_awgn_power_for(level, signal_power, rate);
    if (!power) {
        return std::unexpected(power.error());
    }

    // One component, not two. A complex sample splits its noise power across
    // two quadratures and a real sample does not, so the variance here is the
    // total power rather than half of it. docs/snr-convention.md puts the
    // cost of getting that wrong at 3 dB.
    const double sigma = std::sqrt(*power);

    std::mt19937_64 engine(derive_seed(seed, kDomainRealAwgn));
    for (std::size_t i = 0; i < composite.size(); i += 2) {
        const GaussianPair pair = next_gaussian_pair(engine);
        composite[i] = static_cast<float>(static_cast<double>(composite[i]) + sigma * pair.first);
        if (i + 1 < composite.size()) {
            composite[i + 1] =
                static_cast<float>(static_cast<double>(composite[i + 1]) + sigma * pair.second);
        }
    }

    RdsNoiseReport report;
    report.signal_power = signal_power;
    report.noise_power = *power;
    report.reference_bandwidth_hz = (level.basis == SnrBasis::ReferenceBandwidth)
                                        ? level.reference_bandwidth_hz
                                        : kWsjtxReferenceBandwidthHz;
    report.snr_in_full_band_db = 10.0 * std::log10(signal_power / *power);

    Expected<double> reference = full_band_to_reference_bandwidth_db(
        report.snr_in_full_band_db, rate / 2, report.reference_bandwidth_hz);
    if (!reference) {
        return std::unexpected(reference.error());
    }
    report.snr_in_reference_bandwidth_db = *reference;
    report.noise_power_in_reference_bandwidth =
        *power * static_cast<double>(report.reference_bandwidth_hz) /
        (static_cast<double>(rate) / 2.0);

    Expected<double> eb_n0 =
        full_band_to_eb_over_n0_db(report.snr_in_full_band_db, rate / 2, decode::kBitRateHz);
    if (!eb_n0) {
        return std::unexpected(eb_n0.error());
    }
    report.eb_over_n0_db = *eb_n0;

    return report;
}

Expected<SnrMeasurement> measure_real_snr(dsp::ConstRealSpan clean,
                                          dsp::ConstRealSpan impaired,
                                          double signal_power,
                                          SampleRate rate,
                                          double bits_per_second)
{
    if (rate <= 1) {
        return fail(std::format("measure_real_snr needs a positive sample rate, got {}", rate));
    }
    if (clean.size() != impaired.size()) {
        return fail(std::format(
            "measure_real_snr needs matching lengths, got {} clean and {} impaired",
            clean.size(), impaired.size()));
    }
    if (clean.empty()) {
        return fail("measure_real_snr was given an empty buffer");
    }
    if (!(signal_power > 0.0)) {
        return fail("measure_real_snr has no SNR to report against a signal of zero power");
    }

    double noise_total = 0.0;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        const double noise =
            static_cast<double>(impaired[i]) - static_cast<double>(clean[i]);
        noise_total += noise * noise;
    }

    SnrMeasurement measurement;
    measurement.signal_power = signal_power;
    measurement.noise_power = noise_total / static_cast<double>(clean.size());
    measurement.snr_in_full_band_db =
        (measurement.noise_power > 0.0)
            ? 10.0 * std::log10(signal_power / measurement.noise_power)
            : std::numeric_limits<double>::infinity();

    // rate/2, because a real stream at `rate` represents rate/2 hertz of
    // spectrum. Same substitution add_real_awgn makes, and it has to be the
    // same one or the two figures stop being comparable.
    Expected<double> reference =
        full_band_to_reference_bandwidth_db(measurement.snr_in_full_band_db, rate / 2);
    if (!reference) {
        return std::unexpected(with_context(reference.error(), "measure_real_snr"));
    }
    measurement.snr_in_2500_hz_db = *reference;

    if (bits_per_second > 0.0) {
        Expected<double> eb_n0 = full_band_to_eb_over_n0_db(measurement.snr_in_full_band_db,
                                                            rate / 2, bits_per_second);
        if (!eb_n0) {
            return std::unexpected(with_context(eb_n0.error(), "measure_real_snr"));
        }
        measurement.eb_over_n0_db = *eb_n0;
        measurement.measured_eb_over_n0 = true;
    }

    return measurement;
}

}  // namespace revenant::siggen
