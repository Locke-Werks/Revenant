// Channel simulator implementation.
//
// reference_fp.h comes first and must stay first: its pragma has to precede
// every function definition in the translation unit. Contraction is off here
// for the same reason it is off in the DSP reference twins. A channel that
// fuses its multiply-adds on one host compiler and not another produces
// different samples from the same seed, and a seed that does not reproduce is
// not a seed.

#include "core/dsp/reference_fp.h"

#include "core/dsp/synth/channel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

static_assert(revenant::dsp::kReferenceFpDisciplineApplied,
              "channel.cpp must be compiled with floating-point contraction disabled");

namespace revenant::siggen {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647693;

// Ten for a power ratio, twenty for an amplitude ratio. Named so the two are
// never confused at a call site, which is a classic source of a silent 2x
// error in decibels.
constexpr double kPowerDecibelScale = 10.0;
constexpr double kAmplitudeDecibelScale = 20.0;

// Per-stage seed domains. The values only have to be distinct; derive_seed
// scatters them.
constexpr std::uint64_t kDomainPropagation = 0x01;
constexpr std::uint64_t kDomainFrequency = 0x02;
constexpr std::uint64_t kDomainImpulse = 0x03;
constexpr std::uint64_t kDomainAwgn = 0x04;
constexpr std::uint64_t kDomainInterfererBase = 0x0100;
constexpr std::uint64_t kDomainTapBase = 0x1000;

// Half-length of the band-limited interpolator used for fractional delay and
// for resampling under clock error. Sixteen taps of a Kaiser-windowed sinc put
// the interpolation error below -90 dB across the inner 80 percent of the
// band, which is well under any noise floor this simulator is used at.
constexpr int kSincHalfLength = 8;
constexpr int kSincTapCount = 2 * kSincHalfLength;
constexpr double kKaiserBeta = 8.6;

// Number of scattered arrivals in the fading process. Zheng and Xiao show the
// autocorrelation and the envelope statistics converge by about sixteen; using
// thirty two costs nothing here and tightens the second-order statistics.
constexpr int kFadingPathCount = 32;

// Guards on inputs, so a typo fails with a message instead of allocating for
// an hour.
constexpr double kMaxClockErrorPpm = 100000.0;
constexpr double kMaxInitialOffsetSamples = 1.0e6;
constexpr double kMaxLogNormalSigmaDb = 60.0;
constexpr double kMaxDelaySamples = 1.0e9;

[[nodiscard]] double db_to_power_ratio(double db)
{
    return std::pow(10.0, db / kPowerDecibelScale);
}

[[nodiscard]] double db_to_amplitude_ratio(double db)
{
    return std::pow(10.0, db / kAmplitudeDecibelScale);
}

// A ratio of two powers in decibels, with the degenerate cases given their
// mathematical answers rather than a NaN that propagates into a report.
[[nodiscard]] double power_ratio_db(double numerator, double denominator)
{
    if (denominator <= 0.0)
    {
        return std::numeric_limits<double>::infinity();
    }
    if (numerator <= 0.0)
    {
        return -std::numeric_limits<double>::infinity();
    }
    return kPowerDecibelScale * std::log10(numerator / denominator);
}

[[nodiscard]] double wrap_phase(double phase)
{
    if (phase > kPi || phase < -kPi)
    {
        return std::remainder(phase, kTwoPi);
    }
    return phase;
}

// Uniform in (0, 1], taken from the top 53 bits of the engine output.
//
// std::uniform_real_distribution is avoided on purpose: the standard does not
// specify how a distribution consumes its engine, so the same seed gives
// different numbers under different standard libraries. The same goes for
// std::normal_distribution, hence Box-Muller below. Everything random in this
// file reduces to raw mt19937_64 output and arithmetic that is specified bit
// for bit.
[[nodiscard]] double next_uniform(std::mt19937_64& engine)
{
    constexpr double kInverse2Pow53 = 1.0 / 9007199254740992.0;
    const std::uint64_t bits = engine() >> 11;
    return (static_cast<double>(bits) + 1.0) * kInverse2Pow53;
}

[[nodiscard]] double next_phase(std::mt19937_64& engine)
{
    return kTwoPi * next_uniform(engine) - kPi;
}

struct GaussianPair
{
    double first = 0.0;
    double second = 0.0;
};

// Box-Muller. Both outputs are used everywhere they are drawn, so nothing is
// cached and the engine consumption pattern stays obvious.
[[nodiscard]] GaussianPair next_gaussian_pair(std::mt19937_64& engine)
{
    const double uniform_radius = next_uniform(engine);
    const double uniform_angle = next_uniform(engine);
    const double radius = std::sqrt(-2.0 * std::log(uniform_radius));
    const double angle = kTwoPi * uniform_angle;
    return GaussianPair{radius * std::cos(angle), radius * std::sin(angle)};
}

// Modified Bessel function of the first kind, order zero, by its power series.
// Only needed for the Kaiser window, where the argument never exceeds beta.
[[nodiscard]] double bessel_i0(double x)
{
    const double quarter_square = 0.25 * x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 64; ++k)
    {
        const double index = static_cast<double>(k);
        term *= quarter_square / (index * index);
        sum += term;
        if (term < sum * 1.0e-17)
        {
            break;
        }
    }
    return sum;
}

[[nodiscard]] double sinc(double x)
{
    if (std::abs(x) < 1.0e-12)
    {
        return 1.0;
    }
    const double scaled = kPi * x;
    return std::sin(scaled) / scaled;
}

using SincTaps = std::array<double, static_cast<std::size_t>(kSincTapCount)>;

// Band-limited interpolation onto a non-integer sample position.
class SincInterpolator
{
public:
    SincInterpolator()
        : inverse_i0_beta_(1.0 / bessel_i0(kKaiserBeta))
    {
    }

    // fraction is in [0, 1). taps[j] multiplies input[base + j - kSincHalfLength + 1].
    void taps_for(double fraction, SincTaps& taps) const
    {
        for (std::size_t j = 0; j < taps.size(); ++j)
        {
            const double offset =
                static_cast<double>(static_cast<int>(j) - kSincHalfLength + 1) - fraction;
            const double ratio = offset / static_cast<double>(kSincHalfLength);
            if (ratio <= -1.0 || ratio >= 1.0)
            {
                taps[j] = 0.0;
                continue;
            }
            const double window =
                bessel_i0(kKaiserBeta * std::sqrt(1.0 - ratio * ratio)) * inverse_i0_beta_;
            taps[j] = sinc(offset) * window;
        }
    }

    [[nodiscard]] Complex32 sample_at(ConstComplexSpan input, double position) const
    {
        const double base = std::floor(position);
        SincTaps taps{};
        taps_for(position - base, taps);
        return apply_taps(input, static_cast<std::int64_t>(base), taps);
    }

    // Samples outside the buffer read as zero, which is what a receiver that
    // had not started capturing yet would have seen.
    [[nodiscard]] static Complex32 apply_taps(ConstComplexSpan input,
                                              std::int64_t base,
                                              const SincTaps& taps)
    {
        const auto count = static_cast<std::int64_t>(input.size());
        double accumulated_real = 0.0;
        double accumulated_imag = 0.0;
        for (std::size_t j = 0; j < taps.size(); ++j)
        {
            const std::int64_t index =
                base + static_cast<std::int64_t>(j) - kSincHalfLength + 1;
            if (index < 0 || index >= count)
            {
                continue;
            }
            const Complex32 value = input[static_cast<std::size_t>(index)];
            accumulated_real += taps[j] * static_cast<double>(value.real());
            accumulated_imag += taps[j] * static_cast<double>(value.imag());
        }
        return Complex32{static_cast<float>(accumulated_real),
                         static_cast<float>(accumulated_imag)};
    }

private:
    double inverse_i0_beta_;
};

// Delays a buffer by a fixed, possibly fractional, number of samples. The
// fractional part is constant across the buffer, so the interpolation taps are
// computed once rather than per sample.
[[nodiscard]] std::vector<Complex32> delay_signal(ConstComplexSpan input,
                                                  double delay_samples,
                                                  const SincInterpolator& interpolator)
{
    std::vector<Complex32> output(input.size());
    if (input.empty())
    {
        return output;
    }

    const double rounded = std::round(delay_samples);
    if (std::abs(delay_samples - rounded) < 1.0e-12)
    {
        // An integer delay is a copy. Taking the interpolator path instead
        // would leave taps at the 1e-17 level from sin(pi*k) not being exactly
        // zero, which is harmless but makes an exactness test fail for no good
        // reason.
        const auto shift = static_cast<std::int64_t>(rounded);
        const auto count = static_cast<std::int64_t>(input.size());
        for (std::int64_t n = 0; n < count; ++n)
        {
            const std::int64_t source = n - shift;
            if (source < 0 || source >= count)
            {
                continue;
            }
            output[static_cast<std::size_t>(n)] = input[static_cast<std::size_t>(source)];
        }
        return output;
    }

    const double base = std::floor(-delay_samples);
    SincTaps taps{};
    interpolator.taps_for(-delay_samples - base, taps);
    const auto base_index = static_cast<std::int64_t>(base);
    for (std::size_t n = 0; n < output.size(); ++n)
    {
        output[n] = SincInterpolator::apply_taps(input, base_index + static_cast<std::int64_t>(n),
                                                 taps);
    }
    return output;
}

// A Kaiser-windowed lowpass, cutoff in cycles per sample.
[[nodiscard]] std::vector<double> kaiser_lowpass(double normalized_cutoff, int half_length)
{
    const double inverse_i0_beta = 1.0 / bessel_i0(kKaiserBeta);
    const auto count = static_cast<std::size_t>(2 * half_length + 1);
    std::vector<double> taps(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const double offset = static_cast<double>(static_cast<int>(i) - half_length);
        const double ratio = offset / static_cast<double>(half_length);
        const double window =
            bessel_i0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) *
            inverse_i0_beta;
        taps[i] = 2.0 * normalized_cutoff * sinc(2.0 * normalized_cutoff * offset) * window;
    }
    return taps;
}

[[nodiscard]] std::vector<std::complex<double>> convolve_same(
    const std::vector<std::complex<double>>& input,
    const std::vector<double>& taps)
{
    const auto length = static_cast<std::ptrdiff_t>(input.size());
    const auto tap_count = static_cast<std::ptrdiff_t>(taps.size());
    const std::ptrdiff_t center = tap_count / 2;
    std::vector<std::complex<double>> output(input.size());
    for (std::ptrdiff_t i = 0; i < length; ++i)
    {
        std::complex<double> accumulated{0.0, 0.0};
        for (std::ptrdiff_t j = 0; j < tap_count; ++j)
        {
            const std::ptrdiff_t index = i + center - j;
            if (index < 0 || index >= length)
            {
                continue;
            }
            accumulated += input[static_cast<std::size_t>(index)] * taps[static_cast<std::size_t>(j)];
        }
        output[static_cast<std::size_t>(i)] = accumulated;
    }
    return output;
}

struct PreparedTap
{
    double delay_samples = 0.0;
    double gain_linear = 1.0;
    double phase_radians = 0.0;
};

[[nodiscard]] Expected<std::vector<PreparedTap>> prepare_taps(const MultipathProfile& profile,
                                                              SampleRate rate)
{
    if (rate <= 0)
    {
        return fail(std::format("multipath needs a positive sample rate, got {}", rate));
    }

    std::vector<MultipathTap> source = profile.taps;
    if (source.empty())
    {
        source.push_back(MultipathTap{});
    }

    std::vector<PreparedTap> prepared;
    prepared.reserve(source.size());
    double total_power = 0.0;

    for (std::size_t i = 0; i < source.size(); ++i)
    {
        const MultipathTap& tap = source[i];
        if (!std::isfinite(tap.delay_seconds) || tap.delay_seconds < 0.0)
        {
            return fail(std::format("multipath tap {} has delay_seconds {}, which must be "
                                    "finite and not negative",
                                    i, tap.delay_seconds));
        }
        if (!std::isfinite(tap.gain_db) || !std::isfinite(tap.phase_radians))
        {
            return fail(std::format("multipath tap {} has a non-finite gain or phase", i));
        }

        PreparedTap entry;
        entry.delay_samples = tap.delay_seconds * static_cast<double>(rate);
        if (entry.delay_samples > kMaxDelaySamples)
        {
            return fail(std::format("multipath tap {} delays by {} samples, past the sane limit "
                                    "of {}",
                                    i, entry.delay_samples, kMaxDelaySamples));
        }
        entry.gain_linear = db_to_amplitude_ratio(tap.gain_db);
        entry.phase_radians = tap.phase_radians;
        total_power += entry.gain_linear * entry.gain_linear;
        prepared.push_back(entry);
    }

    if (profile.normalize_power)
    {
        if (!(total_power > 0.0))
        {
            return fail("multipath profile has no power in it, so it cannot be normalised");
        }
        const double scale = 1.0 / std::sqrt(total_power);
        for (PreparedTap& entry : prepared)
        {
            entry.gain_linear *= scale;
        }
    }

    return prepared;
}

}  // namespace

// ---------------------------------------------------------------------------
// Seeding
// ---------------------------------------------------------------------------

std::uint64_t derive_seed(std::uint64_t master_seed, std::uint64_t domain)
{
    // SplitMix64 finaliser. Unsigned overflow is defined, which is why the
    // whole mixer is written on uint64_t.
    std::uint64_t state = master_seed + domain * 0x9E3779B97F4A7C15ULL;
    state = (state ^ (state >> 30)) * 0xBF58476D1CE4E5B9ULL;
    state = (state ^ (state >> 27)) * 0x94D049BB133111EBULL;
    return state ^ (state >> 31);
}

// ---------------------------------------------------------------------------
// SNR conventions
// ---------------------------------------------------------------------------

NoiseLevel NoiseLevel::snr_in_2500_hz_db(double db)
{
    NoiseLevel level;
    level.basis = SnrBasis::ReferenceBandwidth;
    level.value_db = db;
    level.reference_bandwidth_hz = kWsjtxReferenceBandwidthHz;
    return level;
}

NoiseLevel NoiseLevel::snr_in_reference_bandwidth_db(double db, Hertz bandwidth_hz)
{
    NoiseLevel level;
    level.basis = SnrBasis::ReferenceBandwidth;
    level.value_db = db;
    level.reference_bandwidth_hz = bandwidth_hz;
    return level;
}

NoiseLevel NoiseLevel::snr_in_full_sample_rate_db(double db)
{
    NoiseLevel level;
    level.basis = SnrBasis::FullSampleRate;
    level.value_db = db;
    return level;
}

NoiseLevel NoiseLevel::eb_over_n0_db(double db, double bits_per_second)
{
    NoiseLevel level;
    level.basis = SnrBasis::EbN0;
    level.value_db = db;
    level.bits_per_second = bits_per_second;
    return level;
}

Expected<double> NoiseLevel::to_full_sample_rate_db(SampleRate rate) const
{
    if (!std::isfinite(value_db))
    {
        return fail("noise level is not a finite number of decibels");
    }
    switch (basis)
    {
    case SnrBasis::ReferenceBandwidth:
        return reference_bandwidth_to_full_band_db(value_db, rate, reference_bandwidth_hz);
    case SnrBasis::FullSampleRate:
        if (rate <= 0)
        {
            return fail(std::format("sample rate must be positive, got {}", rate));
        }
        return value_db;
    case SnrBasis::EbN0:
        return eb_over_n0_to_full_band_db(value_db, rate, bits_per_second);
    }
    return fail("noise level carries an unknown SNR basis");
}

Expected<double> reference_bandwidth_to_full_band_db(double snr_in_reference_bandwidth_db,
                                                     SampleRate rate,
                                                     Hertz reference_bandwidth_hz)
{
    if (rate <= 0)
    {
        return fail(std::format("sample rate must be positive, got {}", rate));
    }
    if (reference_bandwidth_hz <= 0)
    {
        return fail(std::format("reference bandwidth must be positive, got {} Hz",
                                reference_bandwidth_hz));
    }
    // SNR_full_dB = SNR_B_dB - 10*log10(fs / B)
    const double ratio =
        static_cast<double>(rate) / static_cast<double>(reference_bandwidth_hz);
    return snr_in_reference_bandwidth_db - kPowerDecibelScale * std::log10(ratio);
}

Expected<double> full_band_to_reference_bandwidth_db(double snr_in_full_band_db,
                                                     SampleRate rate,
                                                     Hertz reference_bandwidth_hz)
{
    if (rate <= 0)
    {
        return fail(std::format("sample rate must be positive, got {}", rate));
    }
    if (reference_bandwidth_hz <= 0)
    {
        return fail(std::format("reference bandwidth must be positive, got {} Hz",
                                reference_bandwidth_hz));
    }
    // SNR_B_dB = SNR_full_dB + 10*log10(fs / B)
    const double ratio =
        static_cast<double>(rate) / static_cast<double>(reference_bandwidth_hz);
    return snr_in_full_band_db + kPowerDecibelScale * std::log10(ratio);
}

Expected<double> full_band_to_eb_over_n0_db(double snr_in_full_band_db,
                                            SampleRate rate,
                                            double bits_per_second)
{
    if (rate <= 0)
    {
        return fail(std::format("sample rate must be positive, got {}", rate));
    }
    if (!(bits_per_second > 0.0) || !std::isfinite(bits_per_second))
    {
        return fail(std::format("Eb/N0 needs a positive information bit rate, got {}",
                                bits_per_second));
    }
    // EbN0_dB = SNR_full_dB + 10*log10(fs / Rb)
    return snr_in_full_band_db +
           kPowerDecibelScale * std::log10(static_cast<double>(rate) / bits_per_second);
}

Expected<double> eb_over_n0_to_full_band_db(double eb_over_n0_db,
                                            SampleRate rate,
                                            double bits_per_second)
{
    if (rate <= 0)
    {
        return fail(std::format("sample rate must be positive, got {}", rate));
    }
    if (!(bits_per_second > 0.0) || !std::isfinite(bits_per_second))
    {
        return fail(std::format("Eb/N0 needs a positive information bit rate, got {}",
                                bits_per_second));
    }
    // SNR_full_dB = EbN0_dB - 10*log10(fs / Rb)
    return eb_over_n0_db -
           kPowerDecibelScale * std::log10(static_cast<double>(rate) / bits_per_second);
}

Expected<double> reference_bandwidth_to_eb_over_n0_db(double snr_in_reference_bandwidth_db,
                                                      double bits_per_second,
                                                      Hertz reference_bandwidth_hz)
{
    if (reference_bandwidth_hz <= 0)
    {
        return fail(std::format("reference bandwidth must be positive, got {} Hz",
                                reference_bandwidth_hz));
    }
    if (!(bits_per_second > 0.0) || !std::isfinite(bits_per_second))
    {
        return fail(std::format("Eb/N0 needs a positive information bit rate, got {}",
                                bits_per_second));
    }
    // EbN0_dB = SNR_B_dB + 10*log10(B / Rb). No sample rate appears: both sides
    // describe the channel rather than the buffer it was written into.
    return snr_in_reference_bandwidth_db +
           kPowerDecibelScale *
               std::log10(static_cast<double>(reference_bandwidth_hz) / bits_per_second);
}

Expected<double> eb_over_n0_to_reference_bandwidth_db(double eb_over_n0_db,
                                                      double bits_per_second,
                                                      Hertz reference_bandwidth_hz)
{
    if (reference_bandwidth_hz <= 0)
    {
        return fail(std::format("reference bandwidth must be positive, got {} Hz",
                                reference_bandwidth_hz));
    }
    if (!(bits_per_second > 0.0) || !std::isfinite(bits_per_second))
    {
        return fail(std::format("Eb/N0 needs a positive information bit rate, got {}",
                                bits_per_second));
    }
    // SNR_B_dB = EbN0_dB - 10*log10(B / Rb)
    return eb_over_n0_db -
           kPowerDecibelScale *
               std::log10(static_cast<double>(reference_bandwidth_hz) / bits_per_second);
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

double mean_power(ConstComplexSpan samples)
{
    if (samples.empty())
    {
        return 0.0;
    }
    double total = 0.0;
    for (const Complex32& sample : samples)
    {
        const double real_part = static_cast<double>(sample.real());
        const double imag_part = static_cast<double>(sample.imag());
        total += real_part * real_part + imag_part * imag_part;
    }
    return total / static_cast<double>(samples.size());
}

Expected<SnrMeasurement> measure_snr(ConstComplexSpan clean,
                                     ConstComplexSpan impaired,
                                     SampleRate rate,
                                     double bits_per_second)
{
    if (rate <= 0)
    {
        return fail(std::format("sample rate must be positive, got {}", rate));
    }
    if (clean.size() != impaired.size())
    {
        return fail(std::format("measure_snr needs matching lengths, got {} clean and {} impaired",
                                clean.size(), impaired.size()));
    }
    if (clean.empty())
    {
        return fail("measure_snr was given an empty buffer");
    }

    double signal_total = 0.0;
    double noise_total = 0.0;
    for (std::size_t i = 0; i < clean.size(); ++i)
    {
        const double clean_real = static_cast<double>(clean[i].real());
        const double clean_imag = static_cast<double>(clean[i].imag());
        const double noise_real = static_cast<double>(impaired[i].real()) - clean_real;
        const double noise_imag = static_cast<double>(impaired[i].imag()) - clean_imag;
        signal_total += clean_real * clean_real + clean_imag * clean_imag;
        noise_total += noise_real * noise_real + noise_imag * noise_imag;
    }

    const double count = static_cast<double>(clean.size());
    SnrMeasurement measurement;
    measurement.signal_power = signal_total / count;
    measurement.noise_power = noise_total / count;
    measurement.snr_in_full_band_db =
        power_ratio_db(measurement.signal_power, measurement.noise_power);

    auto reference = full_band_to_reference_bandwidth_db(measurement.snr_in_full_band_db, rate);
    if (!reference)
    {
        return std::unexpected(with_context(reference.error(), "measure_snr"));
    }
    measurement.snr_in_2500_hz_db = *reference;

    if (bits_per_second > 0.0)
    {
        auto eb_n0 =
            full_band_to_eb_over_n0_db(measurement.snr_in_full_band_db, rate, bits_per_second);
        if (!eb_n0)
        {
            return std::unexpected(with_context(eb_n0.error(), "measure_snr"));
        }
        measurement.eb_over_n0_db = *eb_n0;
        measurement.measured_eb_over_n0 = true;
    }

    return measurement;
}

// ---------------------------------------------------------------------------
// AWGN
// ---------------------------------------------------------------------------

Expected<double> awgn_power_for(const NoiseLevel& level, double signal_power, SampleRate rate)
{
    if (!(signal_power > 0.0) || !std::isfinite(signal_power))
    {
        return fail(std::format("SNR is calibrated against the measured signal power, which came "
                                "out as {}. An all-zero or non-finite buffer has no SNR.",
                                signal_power));
    }
    auto full_band_db = level.to_full_sample_rate_db(rate);
    if (!full_band_db)
    {
        return std::unexpected(with_context(full_band_db.error(), "awgn_power_for"));
    }
    // N = S / 10^(SNR_full_dB / 10)
    return signal_power / db_to_power_ratio(*full_band_db);
}

Status add_awgn_at_power(ComplexSpan signal, double noise_power_full_band, std::uint64_t seed)
{
    if (!std::isfinite(noise_power_full_band) || noise_power_full_band < 0.0)
    {
        return fail(std::format("noise power must be finite and not negative, got {}",
                                noise_power_full_band));
    }
    if (noise_power_full_band == 0.0 || signal.empty())
    {
        return {};
    }

    // A complex sample carries its noise power across two independent
    // quadratures, so each component gets half the total variance. Getting this
    // factor wrong is a 3 dB error in every published number.
    const double sigma = std::sqrt(noise_power_full_band * 0.5);
    std::mt19937_64 engine(seed);
    for (Complex32& sample : signal)
    {
        const GaussianPair pair = next_gaussian_pair(engine);
        sample += Complex32{static_cast<float>(sigma * pair.first),
                            static_cast<float>(sigma * pair.second)};
    }
    return {};
}

Expected<NoiseReport> add_awgn(ComplexSpan signal,
                               const NoiseLevel& level,
                               SampleRate rate,
                               std::uint64_t seed)
{
    const double signal_power = mean_power(signal);
    auto noise_power = awgn_power_for(level, signal_power, rate);
    if (!noise_power)
    {
        return std::unexpected(with_context(noise_power.error(), "add_awgn"));
    }
    Status added = add_awgn_at_power(signal, *noise_power, seed);
    if (!added)
    {
        return std::unexpected(with_context(added.error(), "add_awgn"));
    }

    NoiseReport report;
    report.signal_power = signal_power;
    report.noise_power_full_band = *noise_power;
    report.reference_bandwidth_hz = (level.basis == SnrBasis::ReferenceBandwidth)
                                        ? level.reference_bandwidth_hz
                                        : kWsjtxReferenceBandwidthHz;
    report.noise_power_in_reference_bandwidth =
        *noise_power * static_cast<double>(report.reference_bandwidth_hz) /
        static_cast<double>(rate);
    report.snr_in_full_band_db = power_ratio_db(signal_power, *noise_power);

    auto reference = full_band_to_reference_bandwidth_db(report.snr_in_full_band_db, rate,
                                                         report.reference_bandwidth_hz);
    if (!reference)
    {
        return std::unexpected(with_context(reference.error(), "add_awgn"));
    }
    report.snr_in_reference_bandwidth_db = *reference;
    return report;
}

// ---------------------------------------------------------------------------
// Fading
// ---------------------------------------------------------------------------

Status apply_fading(ComplexSpan signal,
                    const FadingConfig& config,
                    SampleRate rate,
                    std::uint64_t seed)
{
    if (config.model == FadingModel::None || signal.empty())
    {
        return {};
    }
    if (rate <= 0)
    {
        return fail(std::format("fading needs a positive sample rate, got {}", rate));
    }
    if (!std::isfinite(config.doppler_spread_hz) || config.doppler_spread_hz < 0.0)
    {
        return fail(std::format("doppler_spread_hz must be finite and not negative, got {}",
                                config.doppler_spread_hz));
    }
    const double sample_rate = static_cast<double>(rate);
    if (config.doppler_spread_hz > sample_rate * 0.5)
    {
        return fail(std::format("doppler_spread_hz {} exceeds Nyquist for a {} Hz stream",
                                config.doppler_spread_hz, rate));
    }
    if (config.model == FadingModel::Rician && !std::isfinite(config.rician_k_db))
    {
        return fail("rician_k_db is not a finite number of decibels");
    }

    std::mt19937_64 engine(seed);

    // Zheng and Xiao's improved sum-of-sinusoids Rayleigh model. The arrival
    // angles are offset by a single shared random rotation, which is what fixes
    // the correlation defects of the classical Jakes construction while keeping
    // the process exactly reproducible from a seed.
    const double angle_offset = next_phase(engine);
    std::array<double, static_cast<std::size_t>(kFadingPathCount)> phase{};
    std::array<double, static_cast<std::size_t>(kFadingPathCount)> increment{};
    std::array<double, static_cast<std::size_t>(kFadingPathCount)> cos_psi{};
    std::array<double, static_cast<std::size_t>(kFadingPathCount)> sin_psi{};

    const double path_count = static_cast<double>(kFadingPathCount);
    for (std::size_t n = 0; n < phase.size(); ++n)
    {
        const double index = static_cast<double>(n) + 1.0;
        const double arrival_angle =
            (kTwoPi * index - kPi + angle_offset) / (4.0 * path_count);
        const double psi = next_phase(engine);
        phase[n] = next_phase(engine);
        increment[n] = kTwoPi * config.doppler_spread_hz * std::cos(arrival_angle) / sample_rate;
        cos_psi[n] = std::cos(psi);
        sin_psi[n] = std::sin(psi);
    }

    // sqrt(2/M) makes E[|diffuse|^2] exactly one, so fading does not move the
    // average received level and therefore does not move the SNR.
    const double diffuse_scale = std::sqrt(2.0 / path_count);

    double specular_amplitude = 0.0;
    double diffuse_amplitude = 1.0;
    if (config.model == FadingModel::Rician)
    {
        const double k_linear = db_to_power_ratio(config.rician_k_db);
        specular_amplitude = std::sqrt(k_linear / (k_linear + 1.0));
        diffuse_amplitude = std::sqrt(1.0 / (k_linear + 1.0));
    }

    double specular_phase = next_phase(engine);
    const double specular_increment =
        kTwoPi * static_cast<double>(config.line_of_sight_doppler_hz) / sample_rate;

    for (Complex32& sample : signal)
    {
        double in_phase = 0.0;
        double quadrature = 0.0;
        for (std::size_t n = 0; n < phase.size(); ++n)
        {
            const double carrier = std::cos(phase[n]);
            in_phase += cos_psi[n] * carrier;
            quadrature += sin_psi[n] * carrier;
            phase[n] = wrap_phase(phase[n] + increment[n]);
        }

        double gain_real = diffuse_amplitude * diffuse_scale * in_phase;
        double gain_imag = diffuse_amplitude * diffuse_scale * quadrature;

        if (config.model == FadingModel::Rician)
        {
            gain_real += specular_amplitude * std::cos(specular_phase);
            gain_imag += specular_amplitude * std::sin(specular_phase);
            specular_phase = wrap_phase(specular_phase + specular_increment);
        }

        const double sample_real = static_cast<double>(sample.real());
        const double sample_imag = static_cast<double>(sample.imag());
        sample = Complex32{
            static_cast<float>(sample_real * gain_real - sample_imag * gain_imag),
            static_cast<float>(sample_real * gain_imag + sample_imag * gain_real)};
    }

    return {};
}

// ---------------------------------------------------------------------------
// Multipath and propagation
// ---------------------------------------------------------------------------

Expected<std::vector<Complex32>> apply_propagation(ConstComplexSpan input,
                                                   const MultipathProfile& multipath,
                                                   const FadingConfig& fading,
                                                   SampleRate rate,
                                                   std::uint64_t seed)
{
    auto taps = prepare_taps(multipath, rate);
    if (!taps)
    {
        return std::unexpected(with_context(taps.error(), "apply_propagation"));
    }

    std::vector<Complex32> output(input.size());
    if (input.empty())
    {
        return output;
    }

    const SincInterpolator interpolator;
    for (std::size_t i = 0; i < taps->size(); ++i)
    {
        const PreparedTap& tap = (*taps)[i];
        std::vector<Complex32> path = delay_signal(input, tap.delay_samples, interpolator);

        if (fading.model != FadingModel::None)
        {
            FadingConfig tap_fading = fading;
            if (i > 0 && tap_fading.model == FadingModel::Rician)
            {
                // The line of sight arrives first by definition, so only the
                // shortest path carries the specular component. Every later
                // arrival is scattered and therefore Rayleigh.
                tap_fading.model = FadingModel::Rayleigh;
            }
            Status faded = apply_fading(ComplexSpan(path), tap_fading, rate,
                                        derive_seed(seed, kDomainTapBase + i));
            if (!faded)
            {
                return std::unexpected(with_context(faded.error(), "apply_propagation"));
            }
        }

        const Complex32 gain{
            static_cast<float>(tap.gain_linear * std::cos(tap.phase_radians)),
            static_cast<float>(tap.gain_linear * std::sin(tap.phase_radians))};
        for (std::size_t n = 0; n < output.size(); ++n)
        {
            output[n] += path[n] * gain;
        }
    }

    return output;
}

Expected<std::vector<Complex32>> apply_multipath(ConstComplexSpan input,
                                                 const MultipathProfile& profile,
                                                 SampleRate rate)
{
    // Delegating rather than duplicating keeps the static-tap case and the
    // faded case from drifting apart.
    return apply_propagation(input, profile, FadingConfig{}, rate, 0);
}

// ---------------------------------------------------------------------------
// Carrier frequency
// ---------------------------------------------------------------------------

Status apply_frequency_offset(ComplexSpan signal,
                              const FrequencyConfig& config,
                              SampleRate rate,
                              std::uint64_t seed)
{
    if (signal.empty())
    {
        return {};
    }
    if (rate <= 0)
    {
        return fail(std::format("frequency offset needs a positive sample rate, got {}", rate));
    }
    if (!std::isfinite(config.drift_hz_per_second) ||
        !std::isfinite(config.random_walk_hz_per_root_second) ||
        config.random_walk_hz_per_root_second < 0.0)
    {
        return fail("frequency drift terms must be finite, and the random walk not negative");
    }
    if (config.doppler_shift_hz == 0 && config.drift_hz_per_second == 0.0 &&
        config.random_walk_hz_per_root_second == 0.0)
    {
        return {};
    }

    const double sample_rate = static_cast<double>(rate);
    const double base_hz = static_cast<double>(config.doppler_shift_hz);

    // A random walk accumulates as the square root of elapsed time, so a step
    // taken every 1/fs seconds has standard deviation sigma*sqrt(1/fs).
    const double walk_step_sigma = config.random_walk_hz_per_root_second / std::sqrt(sample_rate);
    const bool walking = walk_step_sigma > 0.0;

    std::mt19937_64 engine(seed);
    GaussianPair pair{};
    bool pair_spent = true;

    double phase = 0.0;
    double walk_hz = 0.0;

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        const double sample_real = static_cast<double>(signal[n].real());
        const double sample_imag = static_cast<double>(signal[n].imag());
        const double rotation_real = std::cos(phase);
        const double rotation_imag = std::sin(phase);
        signal[n] = Complex32{
            static_cast<float>(sample_real * rotation_real - sample_imag * rotation_imag),
            static_cast<float>(sample_real * rotation_imag + sample_imag * rotation_real)};

        const double elapsed_seconds = static_cast<double>(n) / sample_rate;
        const double instantaneous_hz =
            base_hz + config.drift_hz_per_second * elapsed_seconds + walk_hz;
        phase = wrap_phase(phase + kTwoPi * instantaneous_hz / sample_rate);

        if (walking)
        {
            if (pair_spent)
            {
                pair = next_gaussian_pair(engine);
                walk_hz += walk_step_sigma * pair.first;
                pair_spent = false;
            }
            else
            {
                walk_hz += walk_step_sigma * pair.second;
                pair_spent = true;
            }
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// Sample clock error
// ---------------------------------------------------------------------------

Expected<std::vector<Complex32>> apply_timing_drift(ConstComplexSpan input,
                                                    const TimingConfig& config)
{
    if (!std::isfinite(config.clock_error_ppm) ||
        std::abs(config.clock_error_ppm) > kMaxClockErrorPpm)
    {
        return fail(std::format("clock_error_ppm must be finite and within +/-{}, got {}",
                                kMaxClockErrorPpm, config.clock_error_ppm));
    }
    if (!std::isfinite(config.initial_offset_samples) ||
        std::abs(config.initial_offset_samples) > kMaxInitialOffsetSamples)
    {
        return fail(std::format("initial_offset_samples must be finite and within +/-{}, got {}",
                                kMaxInitialOffsetSamples, config.initial_offset_samples));
    }

    const double step = 1.0 + config.clock_error_ppm * 1.0e-6;
    if (!(step > 0.0))
    {
        return fail(std::format("clock_error_ppm {} inverts the time base", config.clock_error_ppm));
    }

    std::vector<Complex32> output;
    if (input.empty())
    {
        return output;
    }

    if (config.clock_error_ppm == 0.0 && config.initial_offset_samples == 0.0)
    {
        output.assign(input.begin(), input.end());
        return output;
    }

    const double last_position = static_cast<double>(input.size()) - 1.0;
    const double reach = last_position - config.initial_offset_samples;
    if (reach < 0.0)
    {
        // The whole buffer sits before the first output instant.
        return output;
    }

    const double count_double = std::floor(reach / step) + 1.0;
    const auto count = static_cast<std::size_t>(count_double);
    output.resize(count);

    const SincInterpolator interpolator;
    for (std::size_t n = 0; n < count; ++n)
    {
        const double position = config.initial_offset_samples + static_cast<double>(n) * step;
        output[n] = interpolator.sample_at(input, position);
    }

    return output;
}

// ---------------------------------------------------------------------------
// Impulsive noise
// ---------------------------------------------------------------------------

Expected<ImpulseReport> add_impulsive_noise(ComplexSpan signal,
                                            const ImpulsiveNoiseConfig& config,
                                            SampleRate rate,
                                            double reference_amplitude_rms,
                                            std::uint64_t seed)
{
    ImpulseReport report;
    if (signal.empty() || config.events_per_second == 0.0)
    {
        return report;
    }
    if (rate <= 0)
    {
        return fail(std::format("impulsive noise needs a positive sample rate, got {}", rate));
    }
    if (!std::isfinite(config.events_per_second) || config.events_per_second < 0.0)
    {
        return fail(std::format("events_per_second must be finite and not negative, got {}",
                                config.events_per_second));
    }
    if (config.events_per_second > static_cast<double>(rate))
    {
        return fail(std::format("events_per_second {} is above the sample rate {}, which is no "
                                "longer impulsive noise but a raised noise floor",
                                config.events_per_second, rate));
    }
    if (!std::isfinite(config.amplitude_db_relative_to_signal_rms))
    {
        return fail("amplitude_db_relative_to_signal_rms is not a finite number of decibels");
    }
    if (!std::isfinite(config.duration_seconds) || config.duration_seconds < 0.0)
    {
        return fail(std::format("duration_seconds must be finite and not negative, got {}",
                                config.duration_seconds));
    }
    if (!(reference_amplitude_rms > 0.0) || !std::isfinite(reference_amplitude_rms))
    {
        return fail(std::format("impulse amplitude is stated relative to the wanted signal RMS, "
                                "which came out as {}",
                                reference_amplitude_rms));
    }

    const double sample_rate = static_cast<double>(rate);
    const double mean_gap_samples = sample_rate / config.events_per_second;
    const double amplitude =
        reference_amplitude_rms * db_to_amplitude_ratio(config.amplitude_db_relative_to_signal_rms);

    std::size_t width = 1;
    if (config.duration_seconds > 0.0)
    {
        const double requested = std::round(config.duration_seconds * sample_rate);
        width = (requested < 1.0) ? 1 : static_cast<std::size_t>(requested);
        width = std::min(width, signal.size());
    }

    double log_normal_sigma = 0.0;
    if (config.distribution == ImpulseAmplitude::LogNormal)
    {
        if (!std::isfinite(config.log_normal_sigma_db) || config.log_normal_sigma_db < 0.0 ||
            config.log_normal_sigma_db > kMaxLogNormalSigmaDb)
        {
            return fail(std::format("log_normal_sigma_db must be between 0 and {}, got {}",
                                    kMaxLogNormalSigmaDb, config.log_normal_sigma_db));
        }
        // A standard deviation stated in decibels of amplitude becomes natural
        // log units through ln(10)/20.
        log_normal_sigma = config.log_normal_sigma_db * std::log(10.0) / kAmplitudeDecibelScale;
    }

    std::mt19937_64 engine(seed);
    const double length = static_cast<double>(signal.size());
    const std::size_t iteration_cap = signal.size() * 8 + 1024;

    double position = 0.0;
    double added_total = 0.0;

    for (std::size_t iteration = 0; iteration < iteration_cap; ++iteration)
    {
        // Exponential inter-arrival times, which is what a Poisson process is.
        position += -mean_gap_samples * std::log(next_uniform(engine));
        if (!(position < length))
        {
            break;
        }
        const auto start = static_cast<std::size_t>(position);
        ++report.event_count;

        for (std::size_t k = 0; k < width; ++k)
        {
            const std::size_t index = start + k;
            if (index >= signal.size())
            {
                break;
            }

            double impulse_real = 0.0;
            double impulse_imag = 0.0;
            switch (config.distribution)
            {
            case ImpulseAmplitude::Constant:
            {
                const double phase = next_phase(engine);
                impulse_real = amplitude * std::cos(phase);
                impulse_imag = amplitude * std::sin(phase);
                break;
            }
            case ImpulseAmplitude::Rayleigh:
            {
                // Halving the variance per quadrature makes E[|impulse|^2]
                // equal to amplitude^2.
                const GaussianPair pair = next_gaussian_pair(engine);
                const double sigma = amplitude / std::sqrt(2.0);
                impulse_real = sigma * pair.first;
                impulse_imag = sigma * pair.second;
                break;
            }
            case ImpulseAmplitude::LogNormal:
            {
                const GaussianPair pair = next_gaussian_pair(engine);
                const double phase = next_phase(engine);
                // The -sigma^2 term is the bias correction that keeps
                // E[|impulse|^2] equal to amplitude^2: for z ~ N(0,1),
                // E[exp(2*sigma*z)] is exp(2*sigma^2).
                const double magnitude =
                    amplitude * std::exp(log_normal_sigma * pair.first -
                                         log_normal_sigma * log_normal_sigma);
                impulse_real = magnitude * std::cos(phase);
                impulse_imag = magnitude * std::sin(phase);
                break;
            }
            }

            signal[index] += Complex32{static_cast<float>(impulse_real),
                                       static_cast<float>(impulse_imag)};
            added_total += impulse_real * impulse_real + impulse_imag * impulse_imag;
            ++report.affected_samples;
        }
    }

    report.added_power = added_total / length;
    return report;
}

// ---------------------------------------------------------------------------
// Adjacent channel interference
// ---------------------------------------------------------------------------

Expected<double> add_interferer(ComplexSpan signal,
                                const InterfererConfig& config,
                                SampleRate rate,
                                double signal_power,
                                std::uint64_t seed)
{
    if (signal.empty())
    {
        return 0.0;
    }
    if (rate <= 0)
    {
        return fail(std::format("interference needs a positive sample rate, got {}", rate));
    }
    if (!std::isfinite(config.power_db_relative_to_signal))
    {
        return fail("power_db_relative_to_signal is not a finite number of decibels");
    }
    if (!(signal_power > 0.0) || !std::isfinite(signal_power))
    {
        return fail(std::format("interferer power is stated relative to the wanted signal, which "
                                "came out as {}",
                                signal_power));
    }

    const double target_power = signal_power * db_to_power_ratio(config.power_db_relative_to_signal);
    if (!(target_power > 0.0) || !std::isfinite(target_power))
    {
        return 0.0;
    }

    const double sample_rate = static_cast<double>(rate);
    const double offset_increment = kTwoPi * static_cast<double>(config.offset_hz) / sample_rate;
    std::mt19937_64 engine(seed);

    if (config.kind == InterfererKind::Tone)
    {
        const double amplitude = std::sqrt(target_power);
        double phase = next_phase(engine);
        for (Complex32& sample : signal)
        {
            sample += Complex32{static_cast<float>(amplitude * std::cos(phase)),
                                static_cast<float>(amplitude * std::sin(phase))};
            phase = wrap_phase(phase + offset_increment);
        }
        return target_power;
    }

    if (config.bandwidth_hz <= 0)
    {
        return fail(std::format("band-limited interference needs a positive bandwidth, got {} Hz",
                                config.bandwidth_hz));
    }

    std::vector<std::complex<double>> interferer(signal.size());
    for (std::size_t n = 0; n < interferer.size(); ++n)
    {
        const GaussianPair pair = next_gaussian_pair(engine);
        interferer[n] = std::complex<double>{pair.first, pair.second};
    }

    // Below the sample rate the interferer is shaped to its occupied bandwidth;
    // at or above it there is nothing left to filter out.
    if (static_cast<double>(config.bandwidth_hz) < sample_rate)
    {
        constexpr int kInterfererFilterHalfLength = 64;
        const double normalized_cutoff =
            static_cast<double>(config.bandwidth_hz) / (2.0 * sample_rate);
        const std::vector<double> taps =
            kaiser_lowpass(normalized_cutoff, kInterfererFilterHalfLength);
        interferer = convolve_same(interferer, taps);
    }

    // Scaling to a measured power rather than to the filter's nominal gain
    // makes the delivered interference power exact, whatever the filter did to
    // the edges of the buffer.
    double measured = 0.0;
    for (const std::complex<double>& value : interferer)
    {
        measured += value.real() * value.real() + value.imag() * value.imag();
    }
    measured /= static_cast<double>(interferer.size());
    if (!(measured > 0.0))
    {
        return fail("the generated interferer measured zero power, which should not happen");
    }
    const double scale = std::sqrt(target_power / measured);

    double phase = next_phase(engine);
    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        const double real_part = interferer[n].real() * scale;
        const double imag_part = interferer[n].imag() * scale;
        const double rotation_real = std::cos(phase);
        const double rotation_imag = std::sin(phase);
        signal[n] += Complex32{
            static_cast<float>(real_part * rotation_real - imag_part * rotation_imag),
            static_cast<float>(real_part * rotation_imag + imag_part * rotation_real)};
        phase = wrap_phase(phase + offset_increment);
    }

    return target_power;
}

// ---------------------------------------------------------------------------
// The whole chain
// ---------------------------------------------------------------------------

Expected<ChannelOutput> apply_channel(ConstComplexSpan input,
                                      const ChannelConfig& config,
                                      SampleRate rate,
                                      std::uint64_t seed)
{
    if (rate <= 0)
    {
        return fail(std::format("apply_channel needs a positive sample rate, got {}", rate));
    }

    ChannelOutput output;
    output.report.input_signal_power = mean_power(input);
    output.report.reference_bandwidth_hz = (config.noise.basis == SnrBasis::ReferenceBandwidth)
                                               ? config.noise.reference_bandwidth_hz
                                               : kWsjtxReferenceBandwidthHz;

    // Propagation first: the path the wanted signal takes, before anything is
    // added to it.
    auto propagated = apply_propagation(input, config.multipath, config.fading, rate,
                                        derive_seed(seed, kDomainPropagation));
    if (!propagated)
    {
        return std::unexpected(with_context(propagated.error(), "apply_channel"));
    }

    Status carrier = apply_frequency_offset(ComplexSpan(*propagated), config.frequency, rate,
                                            derive_seed(seed, kDomainFrequency));
    if (!carrier)
    {
        return std::unexpected(with_context(carrier.error(), "apply_channel"));
    }

    auto resampled = apply_timing_drift(*propagated, config.timing);
    if (!resampled)
    {
        return std::unexpected(with_context(resampled.error(), "apply_channel"));
    }

    // The clean copy has been through exactly the same propagation, carrier and
    // timing stages, so differencing it against the output isolates what the
    // additive impairments contributed and nothing else.
    output.clean = *resampled;
    output.samples = std::move(*resampled);
    output.report.output_sample_count = output.samples.size();

    if (output.samples.empty())
    {
        return output;
    }

    const double wanted_power = (config.power_reference == SignalPowerReference::InputBuffer)
                                    ? output.report.input_signal_power
                                    : mean_power(output.samples);
    output.report.wanted_signal_power = wanted_power;

    ComplexSpan working(output.samples);

    for (std::size_t i = 0; i < config.interferers.size(); ++i)
    {
        auto added = add_interferer(working, config.interferers[i], rate, wanted_power,
                                    derive_seed(seed, kDomainInterfererBase + i));
        if (!added)
        {
            return std::unexpected(
                with_context(added.error(), std::format("apply_channel: interferer {}", i)));
        }
        output.report.interference_power += *added;
    }

    if (config.impulsive.events_per_second > 0.0)
    {
        auto impulses = add_impulsive_noise(working, config.impulsive, rate,
                                            std::sqrt(wanted_power),
                                            derive_seed(seed, kDomainImpulse));
        if (!impulses)
        {
            return std::unexpected(with_context(impulses.error(), "apply_channel"));
        }
        output.report.impulse_power = impulses->added_power;
        output.report.impulse_event_count = impulses->event_count;
    }

    if (config.add_noise)
    {
        auto noise_power = awgn_power_for(config.noise, wanted_power, rate);
        if (!noise_power)
        {
            return std::unexpected(with_context(noise_power.error(), "apply_channel"));
        }
        Status added = add_awgn_at_power(working, *noise_power, derive_seed(seed, kDomainAwgn));
        if (!added)
        {
            return std::unexpected(with_context(added.error(), "apply_channel"));
        }

        output.report.awgn_power_full_band = *noise_power;
        output.report.awgn_power_in_reference_bandwidth =
            *noise_power * static_cast<double>(output.report.reference_bandwidth_hz) /
            static_cast<double>(rate);
        output.report.requested_snr_in_full_band_db = power_ratio_db(wanted_power, *noise_power);

        auto reference = full_band_to_reference_bandwidth_db(
            output.report.requested_snr_in_full_band_db, rate,
            output.report.reference_bandwidth_hz);
        if (!reference)
        {
            return std::unexpected(with_context(reference.error(), "apply_channel"));
        }
        output.report.requested_snr_in_reference_bandwidth_db = *reference;
    }

    return output;
}

}  // namespace revenant::siggen
