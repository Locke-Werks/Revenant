// This include must come first. It carries the pragma that disables
// floating-point contraction for this translation unit, and a pragma that
// appears after a function definition does not apply to it.
#include "core/dsp/reference_fp.h"

#include "core/dsp/vrx_reference.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <numeric>

#include "core/dsp/denormal_mode.h"

namespace revenant::dsp {
namespace {

static_assert(kReferenceFpDisciplineApplied,
              "core/dsp/reference_fp.h must be included before anything else in a reference "
              "implementation");

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Rounded to float from the same decimal string core/shaders/vrx_demod.comp
// carries, and through the same double-then-float path glslang takes, so both
// sides hold the identical bit pattern.
constexpr float kHalfPiF = static_cast<float>(1.57079632679489661923);
constexpr float kPiF = static_cast<float>(3.14159265358979323846);

// Fitted against long-double atan over [0, 1] by least squares on the odd
// basis t*(c0 + c1*u + ... + c9*u^9) with u = t*t, then evaluated in float32
// to find the worst error: 9.6e-08 radian, which is the float evaluation
// floor rather than the fit's, since atan(1) is 0.785 and 0.785*2^-23 is
// 9.4e-08. The same ten literals appear in the kernel.
constexpr float kAtanC0 = static_cast<float>(0.9999999998);
constexpr float kAtanC1 = static_cast<float>(-0.3333331929);
constexpr float kAtanC2 = static_cast<float>(0.1999923872);
constexpr float kAtanC3 = static_cast<float>(-0.1427217912);
constexpr float kAtanC4 = static_cast<float>(0.1099615976);
constexpr float kAtanC5 = static_cast<float>(-0.0853800324);
constexpr float kAtanC6 = static_cast<float>(0.0603537785);
constexpr float kAtanC7 = static_cast<float>(-0.0336503503);
constexpr float kAtanC8 = static_cast<float>(0.0122847765);
constexpr float kAtanC9 = static_cast<float>(-0.0021090222);

// Default audio rate when VrxParams leaves it at zero. EngineConfig carries
// the same figure; this is the fallback for a planner called without one, not
// a second opinion about what the engine should use.
constexpr SampleRate kDefaultAudioRate = 48'000;

// Stopband targets. The fine filter is the receiver's only selectivity, so it
// gets the higher one; the audio decimation filter only has to keep the
// detector's out-of-band noise out of the audio.
constexpr double kFineAttenuationDb = 80.0;
constexpr double kAudioAttenuationDb = 80.0;

// log2 of the shared NCO circle. Truncating a phase to 16 bits puts the
// spur floor near -96 dBc, which measurement confirms is the highest spur the
// fine stage produces: the polyphase interpolation's own images sit tens of
// decibels below it. Raising this is the one knob that moves that floor.
constexpr std::uint32_t kDefaultNcoLog2 = 16;

// Corner of the AM DC-removal highpass, in hertz. Below the 300 Hz bottom of
// a voice channel with enough margin that the corner region's phase wobble is
// out of the passband, and short enough that the window is a few hundred taps
// rather than a few thousand.
constexpr SampleRate kAmDcCornerHz = 200;

// Polyphase branches. With linear interpolation between adjacent branches the
// residual timing error is second order in 1/P, so 256 buys roughly 100 dB of
// image rejection at 64 KiB of tap table per receiver. Raising it is a
// straight memory-for-images trade and needs no code change.
constexpr std::uint32_t kDefaultPhases = 256;

// Modified Bessel function of the first kind, order zero, from the ascending
// power series I0(x) = sum_k (x^2/4)^k / (k!)^2, Abramowitz and Stegun 9.6.12.
//
// Written out because MSVC does not ship the C++17 special mathematical
// functions: std::cyl_bessel_i does not exist on this toolchain.
// core/dsp/pfb_design.cpp carries the same series for the same reason and the
// same convergence argument: successive terms are in the ratio (x^2/4)/k^2,
// so stopping when a term is below 1e-18 of the running sum puts the whole
// remaining tail below the resolution of a double.
[[nodiscard]] double bessel_i0(double x) {
    const double quarter_square = 0.25 * x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 256; ++k) {
        term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-18 * sum) {
            break;
        }
    }
    return sum;
}

// Kaiser's empirical shape parameter for a stopband attenuation in positive
// decibels. Oppenheim and Schafer eq. 7.62.
[[nodiscard]] double kaiser_beta(double attenuation_db) {
    if (attenuation_db > 50.0) {
        return 0.1102 * (attenuation_db - 8.7);
    }
    if (attenuation_db >= 21.0) {
        const double excess = attenuation_db - 21.0;
        return 0.5842 * std::pow(excess, 0.4) + 0.07886 * excess;
    }
    // Below 21 dB the Kaiser window degenerates to the rectangular window,
    // whose own sidelobes are already at -21 dB.
    return 0.0;
}

// Kaiser's order estimate, Oppenheim and Schafer eq. 7.63: an order of
// (A - 8)/(2.285*dw) reaches A decibels of stopband across a transition of dw
// radians per sample. Returned as a tap count, which is the order plus one.
//
// Empirical and optimistic. core/dsp/pfb_design.cpp measured the filters it
// produces against this same estimate and found them one to two decibels
// short across the range it uses, so a caller reports what this implies and
// does not assert against it.
[[nodiscard]] std::uint32_t kaiser_taps_for(double attenuation_db, double transition_fraction) {
    if (!(transition_fraction > 0.0)) {
        return 0;
    }
    const double transition_radians = kTwoPi * transition_fraction;
    const double order = (attenuation_db - 8.0) / (2.285 * transition_radians);
    if (!(order > 0.0)) {
        return 2;
    }
    return static_cast<std::uint32_t>(std::ceil(order)) + 1U;
}

// The same estimate read backwards: what a given tap count reaches.
[[nodiscard]] double attenuation_reachable(std::uint32_t taps, double transition_fraction) {
    if (taps < 2 || !(transition_fraction > 0.0)) {
        return 0.0;
    }
    const double transition_radians = kTwoPi * transition_fraction;
    return 2.285 * transition_radians * static_cast<double>(taps - 1U) + 8.0;
}

[[nodiscard]] double sinc_pi(double z) {
    if (z == 0.0) {
        return 1.0;
    }
    const double scaled = kPi * z;
    return std::sin(scaled) / scaled;
}

// The Kaiser window of the given length, evaluated at sample i.
[[nodiscard]] double kaiser_window(std::size_t index, std::size_t length, double beta,
                                   double i0_beta) {
    if (length < 2) {
        return 1.0;
    }
    const double position =
        2.0 * static_cast<double>(index) / static_cast<double>(length - 1) - 1.0;
    const double inner = std::max(0.0, 1.0 - position * position);
    return bessel_i0(beta * std::sqrt(inner)) / i0_beta;
}

[[nodiscard]] std::int64_t absolute(std::int64_t value) { return value < 0 ? -value : value; }

// Reduces a rational in place by the greatest common divisor, keeping the
// denominator positive.
void reduce_rational(std::int64_t& numerator, std::int64_t& denominator) {
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    const std::int64_t divisor = std::gcd(numerator, denominator);
    if (divisor > 1) {
        numerator /= divisor;
        denominator /= divisor;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Deterministic transcendentals
// ---------------------------------------------------------------------------

float det_rsqrt(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const float seed = std::bit_cast<float>(0x5F375A86U - (bits >> 1U));

    const float half_value = 0.5F * value;
    const float r1 = seed * (1.5F - half_value * (seed * seed));
    const float r2 = r1 * (1.5F - half_value * (r1 * r1));
    const float r3 = r2 * (1.5F - half_value * (r2 * r2));
    const float r4 = r3 * (1.5F - half_value * (r3 * r3));
    return r4;
}

float det_sqrt(float value) {
    // Written !(value > 0) rather than value <= 0 so that a NaN, for which
    // every comparison is false, takes the zero branch instead of propagating
    // through the iteration.
    if (!(value > 0.0F)) {
        return 0.0F;
    }
    return value * det_rsqrt(value);
}

float det_recip(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const float seed = std::bit_cast<float>(0x7EF311C3U - bits);

    const float r1 = seed * (2.0F - value * seed);
    const float r2 = r1 * (2.0F - value * r1);
    const float r3 = r2 * (2.0F - value * r2);
    const float r4 = r3 * (2.0F - value * r3);
    return r4;
}

float det_atan_unit(float t) {
    const float u = t * t;
    float p = kAtanC9;
    p = p * u + kAtanC8;
    p = p * u + kAtanC7;
    p = p * u + kAtanC6;
    p = p * u + kAtanC5;
    p = p * u + kAtanC4;
    p = p * u + kAtanC3;
    p = p * u + kAtanC2;
    p = p * u + kAtanC1;
    p = p * u + kAtanC0;
    return t * p;
}

float det_atan2(float y, float x) {
    // std::fabs, and the ternaries below, rather than std::fmin and std::fmax:
    // GLSL min and max are defined as the comparison written out, and the
    // library functions carry NaN rules that GLSL does not. On the finite
    // inputs a discriminator produces they agree, and writing the comparison
    // makes that visible rather than assumed.
    const float ay = std::fabs(y);
    const float ax = std::fabs(x);

    if (ay == 0.0F && ax == 0.0F) {
        return 0.0F;
    }

    const float numerator = (ax < ay) ? ax : ay;
    const float denominator = (ay < ax) ? ax : ay;

    const float ratio = numerator * det_recip(denominator);
    float angle = det_atan_unit(ratio);

    if (ay > ax) {
        angle = kHalfPiF - angle;
    }
    if (x < 0.0F) {
        angle = kPiF - angle;
    }

    return (y < 0.0F) ? -angle : angle;
}

// ---------------------------------------------------------------------------
// Exact phase
// ---------------------------------------------------------------------------

std::uint64_t turn_fixed64(std::int64_t numerator, std::int64_t denominator) {
    if (denominator <= 0 || numerator < 0 || numerator >= denominator) {
        return 0;
    }

    // Long division in four base-65536 digits. numerator << 64 fits nothing
    // this toolchain has, and numerator << 16 comfortably fits a signed
    // 64-bit integer for every denominator kMaxPhaseDenominator allows.
    std::int64_t remainder = numerator;
    std::uint64_t quotient = 0;
    for (int step = 0; step < 4; ++step) {
        remainder <<= 16;
        quotient = (quotient << 16) | static_cast<std::uint64_t>(remainder / denominator);
        remainder %= denominator;
    }

    // Rounding up from the largest representable turn overflows to zero,
    // which is the right answer: a whole turn is no turn.
    if (2 * remainder >= denominator) {
        ++quotient;
    }

    return quotient;
}

Expected<std::uint64_t> nco_delta(std::int64_t hz_numerator, std::int64_t hz_denominator,
                                  SampleRate rate) {
    if (hz_denominator == 0) {
        return fail("nco_delta: frequency denominator is zero");
    }
    if (rate <= 0) {
        return fail(std::format("nco_delta: rate must be positive, got {}", rate));
    }

    std::int64_t numerator = hz_numerator;
    std::int64_t denominator = hz_denominator;
    reduce_rational(numerator, denominator);

    const std::int64_t period = denominator * static_cast<std::int64_t>(rate);
    if (period <= 0 || period > kMaxPhaseDenominator) {
        return fail(std::format(
            "nco_delta: reduced period {} exceeds the {} limit the exact-phase arithmetic "
            "works inside; the frequency's denominator is {} and the rate is {}",
            period, kMaxPhaseDenominator, denominator, rate));
    }

    // Reduce into [0, period). A negative frequency lands on its positive
    // complement, which is the same phasor: exp(-j*2*pi*(-f)) equals
    // exp(-j*2*pi*(1-f)).
    std::int64_t reduced = numerator % period;
    if (reduced < 0) {
        reduced += period;
    }
    return turn_fixed64(reduced, period);
}

Expected<std::vector<Complex32>> build_nco_table(std::uint32_t log2_size) {
    if (log2_size > kMaxNcoLog2) {
        return fail(std::format("build_nco_table: log2 size {} exceeds the limit of {}",
                                log2_size, kMaxNcoLog2));
    }

    const std::uint32_t size = 1U << log2_size;
    std::vector<Complex32> table(size, Complex32{});

    if (size == 1) {
        table[0] = Complex32{1.0F, 0.0F};
        return table;
    }

    // Built from the first octant and reflected, exactly as build_twiddles in
    // core/dsp/pfb.h is. Computing W^(K/2) from cos(pi) and sin(pi) instead
    // leaves a residue of about 1.2e-16 in the imaginary part, which turns a
    // free sign flip into a real complex multiply and costs bit-exactness for
    // nothing. The four quadrature entries here are exactly (1,0), (0,-1),
    // (-1,0) and (0,1).
    const auto quarter = size / 4U;
    const auto octant = size / 8U;

    std::vector<double> cosine(size, 0.0);
    std::vector<double> sine(size, 0.0);

    for (std::uint32_t i = 0; i <= octant; ++i) {
        const double angle = kTwoPi * static_cast<double>(i) / static_cast<double>(size);
        cosine[i] = std::cos(angle);
        sine[i] = std::sin(angle);
    }
    for (std::uint32_t i = octant + 1U; i <= quarter; ++i) {
        // Reflect about pi/4, where cos(pi/2 - a) is sin(a) and the other way
        // round. The reflected index is quarter - i, which is below the
        // octant and therefore already final.
        cosine[i] = sine[quarter - i];
        sine[i] = cosine[quarter - i];
    }
    for (std::uint32_t i = quarter + 1U; i < size; ++i) {
        // Rotate by a quarter turn: cos(a + pi/2) is -sin(a) and
        // sin(a + pi/2) is cos(a). Reading i - quarter, which is below i and
        // so already final, walks the remaining three quadrants a quarter at
        // a time out of the one that was computed.
        cosine[i] = -sine[i - quarter];
        sine[i] = cosine[i - quarter];
    }

    for (std::uint32_t i = 0; i < size; ++i) {
        // exp(-j*2*pi*i/K), so the imaginary part carries the minus sign. The
        // zero entries are written as positive zero rather than as the
        // negative zero the negation produces: nothing downstream is wrong
        // either way, but a table whose first entry prints as (1, -0) invites
        // exactly the sign hunt this comment saves.
        const float real = (cosine[i] == 0.0) ? 0.0F : static_cast<float>(cosine[i]);
        const float imaginary = (sine[i] == 0.0) ? 0.0F : static_cast<float>(-sine[i]);
        table[i] = Complex32{real, imaginary};
    }

    return table;
}

// ---------------------------------------------------------------------------
// The fine stage
// ---------------------------------------------------------------------------

Status validate(const VrxFineConfig& config, const VrxFineParams& params) {
    if (config.taps < 2 || config.taps > kMaxFineTaps) {
        return fail(std::format("vrx fine: {} taps is outside [2, {}]", config.taps,
                                kMaxFineTaps));
    }
    if (config.phases < 1 || config.phases > kMaxFinePhases) {
        return fail(std::format("vrx fine: {} phases is outside [1, {}]", config.phases,
                                kMaxFinePhases));
    }
    if (config.nco_log2 > kMaxNcoLog2) {
        return fail(std::format("vrx fine: NCO log2 size {} exceeds {}", config.nco_log2,
                                kMaxNcoLog2));
    }
    if (params.out_rate == 0) {
        return fail("vrx fine: output rate is zero, so the resampler recurrence divides by "
                    "zero");
    }
    if (params.frac0 >= params.out_rate) {
        return fail(std::format("vrx fine: frac0 {} must be below the output rate {}",
                                params.frac0, params.out_rate));
    }

    const std::uint64_t capacity = static_cast<std::uint64_t>(params.chan_mask) + 1U;
    if ((capacity & static_cast<std::uint64_t>(params.chan_mask)) != 0U) {
        return fail(std::format("vrx fine: channel ring mask {} is not a power of two minus "
                                "one, so the wrap the kernel relies on is not a modulo",
                                params.chan_mask));
    }
    const std::uint64_t out_capacity = static_cast<std::uint64_t>(params.out_mask) + 1U;
    if ((out_capacity & static_cast<std::uint64_t>(params.out_mask)) != 0U) {
        return fail(std::format("vrx fine: output ring mask {} is not a power of two minus one",
                                params.out_mask));
    }

    // The two places the kernel's 32-bit arithmetic can overflow where the
    // host's would not. Both are sizing mistakes in the caller rather than
    // anything the kernel could defend against, and both are silent on the
    // device: the recurrence simply produces the wrong input sample.
    const std::uint64_t accumulated = static_cast<std::uint64_t>(params.frac0) +
                                      static_cast<std::uint64_t>(params.count) *
                                          static_cast<std::uint64_t>(params.step_rem);
    if (accumulated > 0xFFFF'FFFFULL) {
        return fail(std::format(
            "vrx fine: frac0 {} plus {} outputs times a step remainder of {} reaches {}, "
            "which overflows the kernel's 32-bit recurrence. Dispatch fewer outputs per "
            "block",
            params.frac0, params.count, params.step_rem, accumulated));
    }
    const std::uint64_t scaled = static_cast<std::uint64_t>(params.out_rate - 1U) *
                                 static_cast<std::uint64_t>(config.phases);
    if (scaled > 0xFFFF'FFFFULL) {
        return fail(std::format(
            "vrx fine: an output rate of {} with {} phases reaches {} in the kernel's phase "
            "selection, which overflows 32 bits",
            params.out_rate, config.phases, scaled));
    }

    return {};
}

Status reference_vrx_fine(const VrxFineConfig& config, const VrxFineParams& params,
                          ConstComplexSpan channel_ring, ConstComplexSpan taps,
                          ConstComplexSpan nco, ComplexSpan fine_ring) {
    if (const Status valid = validate(config, params); !valid) {
        return std::unexpected(with_context(valid.error(), "reference_vrx_fine"));
    }

    const std::size_t wanted_taps =
        static_cast<std::size_t>(config.phases) * static_cast<std::size_t>(config.taps) + 1U;
    if (taps.size() != wanted_taps) {
        return fail(std::format("reference_vrx_fine wants {} tap entries, got {}", wanted_taps,
                                taps.size()));
    }

    const std::size_t wanted_nco = std::size_t{1} << config.nco_log2;
    if (nco.size() != wanted_nco) {
        return fail(std::format("reference_vrx_fine wants {} NCO entries, got {}", wanted_nco,
                                nco.size()));
    }

    const std::size_t channel_capacity = static_cast<std::size_t>(params.chan_mask) + 1U;
    if (channel_ring.size() < static_cast<std::size_t>(params.chan_base) + channel_capacity) {
        return fail(std::format(
            "reference_vrx_fine: channel ring holds {} samples, but this receiver's channel "
            "starts at {} and the mask implies a capacity of {}",
            channel_ring.size(), params.chan_base, channel_capacity));
    }

    const std::size_t out_capacity = static_cast<std::size_t>(params.out_mask) + 1U;
    if (fine_ring.size() < out_capacity) {
        return fail(std::format("reference_vrx_fine: output ring holds {} samples, mask {} "
                                "implies a capacity of {}",
                                fine_ring.size(), params.out_mask, out_capacity));
    }
    if (params.count > out_capacity) {
        // Both sides would alias the block onto itself identically, so a diff
        // would pass while both were wrong.
        return fail(std::format("reference_vrx_fine: {} outputs do not fit an output ring of "
                                "{} samples",
                                params.count, out_capacity));
    }

    if (params.count == 0) {
        return {};
    }

    // The device flushes denormals to zero in fp32 and, on the hardware this
    // project runs on, cannot be told not to. The reference models the
    // hardware rather than the other way round. See core/dsp/denormal_mode.h.
    const ScopedDenormalFlush flush_denormals;

    for (std::uint32_t i = 0; i < params.count; ++i) {
        // Deliberately 32-bit and deliberately allowed to wrap, which is the
        // arithmetic the device performs. Every value here comes from i alone:
        // nothing is carried from the previous output, which is what lets a
        // block be re-rendered from an arbitrary absolute index.
        const std::uint32_t accumulated = params.frac0 + i * params.step_rem;
        const std::uint32_t carry = accumulated / params.out_rate;
        const std::uint32_t frac = accumulated - carry * params.out_rate;
        const std::uint32_t n_rel = i * params.step_whole + carry;

        const std::uint32_t scaled = frac * config.phases;
        const std::uint32_t phase = scaled / params.out_rate;
        const std::uint32_t blend_num = scaled - phase * params.out_rate;

        const float blend = static_cast<float>(blend_num) * params.inv_out_rate;
        const float blend_complement = 1.0F - blend;

        const std::uint32_t base = params.in_offset + n_rel;

        float acc_real = 0.0F;
        float acc_imag = 0.0F;

        // Ascending k, one lerp and one complex multiply-accumulate per tap,
        // matching the kernel's loop exactly. The order is the claim: a
        // reassociated 32-tap complex sum differs in thousands of units in
        // the last place.
        for (std::uint32_t k = 0; k < config.taps; ++k) {
            const std::uint32_t slot = phase + k * config.phases;

            const Complex32 g0 = taps[slot];
            const Complex32 g1 = taps[slot + 1U];

            const float tap_real = g0.real() * blend_complement + g1.real() * blend;
            const float tap_imag = g0.imag() * blend_complement + g1.imag() * blend;

            // (base - k) & chan_mask, unsigned and wrapping, exactly as the
            // kernel writes it. When k exceeds base the subtraction wraps
            // modulo 2^32; the capacity is a power of two dividing 2^32, so
            // masking lands on the sample the unwrapped arithmetic names.
            const std::size_t index =
                static_cast<std::size_t>(params.chan_base) +
                static_cast<std::size_t>((base - k) & params.chan_mask);

            const Complex32 s = channel_ring[index];

            acc_real = acc_real + (s.real() * tap_real - s.imag() * tap_imag);
            acc_imag = acc_imag + (s.real() * tap_imag + s.imag() * tap_real);
        }

        // The output rotation, as a 0.64 fixed-point turn in two words. The
        // kernel has umulExtended and uaddCarry and no 64-bit integer, so it
        // writes the split explicitly; the twin writes the same split rather
        // than a uint64 multiply so the two read line for line, and both are
        // exact integer arithmetic so they agree by construction.
        const std::uint64_t product =
            static_cast<std::uint64_t>(i) * static_cast<std::uint64_t>(params.nco_delta_low);
        const auto product_high = static_cast<std::uint32_t>(product >> 32U);
        const auto product_low = static_cast<std::uint32_t>(product);

        const std::uint32_t phase_low = params.nco_phase_low + product_low;
        const std::uint32_t phase_carry = (phase_low < params.nco_phase_low) ? 1U : 0U;
        const std::uint32_t phase_high =
            params.nco_phase_high + i * params.nco_delta_high + product_high + phase_carry;

        const std::uint32_t nco_index =
            (config.nco_log2 == 0U) ? 0U : (phase_high >> (32U - config.nco_log2));
        const Complex32 rotation = nco[nco_index];

        const float out_real = acc_real * rotation.real() - acc_imag * rotation.imag();
        const float out_imag = acc_real * rotation.imag() + acc_imag * rotation.real();

        fine_ring[static_cast<std::size_t>((params.out_offset + i) & params.out_mask)] =
            Complex32{out_real, out_imag};
    }

    return {};
}

// ---------------------------------------------------------------------------
// The demodulators
// ---------------------------------------------------------------------------

Status validate(const VrxDemodConfig& config, const VrxDemodParams& params) {
    if (config.mode > kDemodCw) {
        return fail(std::format("vrx demod: mode {} is not one of the eight demodulators",
                                config.mode));
    }
    if (config.decimation == 0) {
        return fail("vrx demod: audio decimation is zero");
    }
    if (config.audio_taps == 0 || config.audio_taps > kMaxAudioTaps) {
        return fail(std::format("vrx demod: {} audio taps is outside [1, {}]",
                                config.audio_taps, kMaxAudioTaps));
    }
    if (config.dc_taps == 0 || config.dc_taps > kMaxDcTaps) {
        return fail(std::format("vrx demod: {} DC-removal taps is outside [1, {}]",
                                config.dc_taps, kMaxDcTaps));
    }
    if (config.mode == kDemodRaw && (config.decimation != 1U || config.audio_taps != 1U)) {
        return fail("vrx demod: the raw tap hands out complex baseband unchanged, so it "
                    "cannot carry an audio decimation filter");
    }

    const std::uint64_t capacity = static_cast<std::uint64_t>(params.in_mask) + 1U;
    if ((capacity & static_cast<std::uint64_t>(params.in_mask)) != 0U) {
        return fail(std::format("vrx demod: input ring mask {} is not a power of two minus one",
                                params.in_mask));
    }

    // The span of fine samples one dispatch reads: the decimation stride over
    // the block, plus the decimation filter's support, plus the detector's own
    // history. A ring shorter than that aliases two different samples onto one
    // slot, and the kernel would do the same thing bit for bit, so a diff
    // would pass while both sides read the wrong history.
    const std::uint64_t detector_history =
        (config.mode == kDemodAm) ? static_cast<std::uint64_t>(config.dc_taps) - 1U
        : (config.mode == kDemodNfm || config.mode == kDemodWfm) ? 1U
                                                                 : 0U;
    const std::uint64_t span =
        (params.count == 0)
            ? 0U
            : (static_cast<std::uint64_t>(params.count - 1U) *
                   static_cast<std::uint64_t>(config.decimation) +
               static_cast<std::uint64_t>(config.audio_taps - 1U) + detector_history + 1U);
    if (span > capacity) {
        return fail(std::format(
            "vrx demod: {} audio samples need {} fine samples of history, and the input ring "
            "holds {}",
            params.count, span, capacity));
    }

    return {};
}

Status reference_vrx_demod(const VrxDemodConfig& config, const VrxDemodParams& params,
                           ConstComplexSpan fine_ring, ConstRealSpan weights, RealSpan audio) {
    if (const Status valid = validate(config, params); !valid) {
        return std::unexpected(with_context(valid.error(), "reference_vrx_demod"));
    }

    const std::size_t wanted_weights =
        static_cast<std::size_t>(config.audio_taps) + static_cast<std::size_t>(config.dc_taps);
    if (weights.size() != wanted_weights) {
        return fail(std::format("reference_vrx_demod wants {} weights ({} audio then {} DC), "
                                "got {}",
                                wanted_weights, config.audio_taps, config.dc_taps,
                                weights.size()));
    }

    const std::size_t capacity = static_cast<std::size_t>(params.in_mask) + 1U;
    if (fine_ring.size() < capacity) {
        return fail(std::format("reference_vrx_demod: fine ring holds {} samples, mask {} "
                                "implies a capacity of {}",
                                fine_ring.size(), params.in_mask, capacity));
    }

    const std::size_t components = (config.mode == kDemodRaw) ? 2U : 1U;
    const std::size_t wanted_audio = static_cast<std::size_t>(params.count) * components;
    if (audio.size() != wanted_audio) {
        return fail(std::format("reference_vrx_demod wants {} audio values, got {}",
                                wanted_audio, audio.size()));
    }

    if (params.count == 0) {
        return {};
    }

    const ScopedDenormalFlush flush_denormals;

    const auto sample_at = [&](std::uint32_t index) -> Complex32 {
        return fine_ring[static_cast<std::size_t>(index & params.in_mask)];
    };

    const auto magnitude_at = [&](std::uint32_t index) -> float {
        const Complex32 z = sample_at(index);
        const float square = z.real() * z.real() + z.imag() * z.imag();
        return det_sqrt(square);
    };

    const auto detect = [&](std::uint32_t index) -> float {
        if (config.mode == kDemodAm) {
            // The DC estimate is a fixed weighted mean of the last dc_taps
            // envelope samples, which makes the removal a plain FIR highpass:
            // a pure function of the absolute index, with no state carried
            // between blocks and therefore usable for retroactive decode. A
            // one-pole highpass would be cheaper and would not be.
            float dc = 0.0F;
            for (std::uint32_t w = 0; w < config.dc_taps; ++w) {
                const float term =
                    magnitude_at(index - w) *
                    weights[static_cast<std::size_t>(config.audio_taps) + w];
                dc = dc + term;
            }
            return magnitude_at(index) - dc;
        }

        if (config.mode == kDemodNfm || config.mode == kDemodWfm) {
            // The argument of z[m]*conj(z[m-1]) rather than a difference of
            // two arguments: the difference of two principal values needs an
            // unwrap and the product does not, because the product's argument
            // is already reduced into (-pi, pi], which is the range a
            // discriminator wants.
            const Complex32 z = sample_at(index);
            const Complex32 previous = sample_at(index - 1U);

            const float product_real =
                z.real() * previous.real() + z.imag() * previous.imag();
            const float product_imag =
                z.imag() * previous.real() - z.real() * previous.imag();

            return det_atan2(product_imag, product_real);
        }

        // USB, LSB, DSB and CW. A product detector at complex baseband is a
        // real part; the mode's identity is the fine stage's passband and
        // mix, not anything here.
        return sample_at(index).real();
    };

    for (std::uint32_t i = 0; i < params.count; ++i) {
        const std::uint32_t index = params.in_offset + i * config.decimation;

        if (config.mode == kDemodRaw) {
            const Complex32 z = sample_at(index);
            audio[static_cast<std::size_t>(2U * i)] = z.real() * params.gain;
            audio[static_cast<std::size_t>(2U * i) + 1U] = z.imag() * params.gain;
            continue;
        }

        float accumulated = 0.0F;
        for (std::uint32_t t = 0; t < config.audio_taps; ++t) {
            const float term = detect(index - t) * weights[t];
            accumulated = accumulated + term;
        }

        audio[static_cast<std::size_t>(i)] = accumulated * params.gain;
    }

    return {};
}

// ---------------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------------

Hertz max_channel_bandwidth(const engine::VrxPlacement& placement) {
    if (placement.channel_rate <= 0 || placement.residual_denominator <= 0) {
        return 0;
    }

    const std::int64_t denominator = placement.residual_denominator;
    const std::int64_t offset = absolute(placement.residual_numerator);

    // Fc - 2*|residual|, kept over a common denominator so the comparison is
    // exact and the rounding happens once, downwards, at the end.
    const std::int64_t scaled =
        static_cast<std::int64_t>(placement.channel_rate) * denominator - 2 * offset;
    if (scaled <= 0) {
        return 0;
    }
    return scaled / denominator;
}

Hertz fm_deviation(std::uint32_t mode, Hertz bandwidth) {
    if (bandwidth <= 0) {
        return 0;
    }
    if (mode == kDemodNfm) {
        // Land mobile: 5 kHz peak deviation in a 25 kHz channel.
        return std::max<Hertz>(1, bandwidth / 5);
    }
    if (mode == kDemodWfm) {
        // FM broadcast: 75 kHz peak deviation in a 200 kHz channel.
        return std::max<Hertz>(1, (3 * bandwidth) / 8);
    }
    return 0;
}

Hertz minimum_demod_rate(std::uint32_t mode, Hertz bandwidth, Hertz cw_pitch) {
    if (bandwidth <= 0) {
        return 0;
    }

    // The floor every mode shares: the fine filter's stopband edge is Fd/2 and
    // its passband edge is B/2, so Fd = B would leave no transition band and
    // therefore no filter. 1.5B leaves a transition of B/4.
    const Hertz floor_rate = (3 * bandwidth + 1) / 2;

    switch (mode) {
        case kDemodAm:
            // The envelope of a band of width B carries content out to B.
            return std::max(floor_rate, 2 * bandwidth);
        case kDemodUsb:
        case kDemodLsb:
            // A product detector on a one-sided passband of width B produces
            // audio out to B.
            return std::max(floor_rate, 2 * bandwidth);
        case kDemodCw:
            // The passband sits at the pitch, so its upper edge is
            // pitch + B/2 and that has to fit below Fd/2.
            return std::max(floor_rate, 2 * cw_pitch + bandwidth);
        default:
            // Raw, DSB and both FM modes. A discriminator produces the
            // modulating audio, which is narrower than the channel, and DSB's
            // real part folds a symmetric band onto half its width.
            return floor_rate;
    }
}

float vrx_demod_gain(std::uint32_t mode, SampleRate demod_rate, Hertz deviation) {
    if (mode != kDemodNfm && mode != kDemodWfm) {
        // Raw is a passthrough, AM's envelope is already in the same units as
        // the input, and a product detector's real part is too. Unity keeps
        // every mode on the one convention: a unit-amplitude signal fully
        // modulating its own mode swings the audio to +/-1.
        //
        // DSB is the one mode where that is not the whole story, and the
        // reason is physics rather than a missing constant. Its complex
        // envelope is real and in phase with a carrier that is not being
        // transmitted, so a product detector with no carrier recovery scales
        // the audio by the cosine of the residual phase between the
        // receiver's oscillator and that suppressed carrier. The measured
        // figure at one tuning is 0.743 against a residual phase of -0.733
        // radian, which is exactly its cosine. USB, LSB and CW are not
        // affected: their audio is a rotating phasor, so a constant phase
        // offset moves its phase and not its amplitude. Closing it needs a
        // loop with carried state, which belongs above a kernel.
        return 1.0F;
    }
    if (deviation <= 0 || demod_rate <= 0) {
        return 1.0F;
    }

    // The discriminator returns the phase advance per sample in radians, and
    // a deviation of d hertz advances the phase by 2*pi*d/Fd per sample. So
    // dividing by that puts peak deviation on +/-1, which is also what makes
    // the output independent of signal amplitude.
    const double scale =
        static_cast<double>(demod_rate) / (kTwoPi * static_cast<double>(deviation));
    return static_cast<float>(scale);
}

Expected<std::vector<Complex32>> design_fine_taps(const VrxFineConfig& config,
                                                  SampleRate channel_rate,
                                                  Hertz half_width_hz,
                                                  std::int64_t centre_numerator,
                                                  std::int64_t centre_denominator,
                                                  double attenuation_db) {
    if (config.taps < 2 || config.taps > kMaxFineTaps) {
        return fail(std::format("design_fine_taps: {} taps is outside [2, {}]", config.taps,
                                kMaxFineTaps));
    }
    if (config.phases < 1 || config.phases > kMaxFinePhases) {
        return fail(std::format("design_fine_taps: {} phases is outside [1, {}]",
                                config.phases, kMaxFinePhases));
    }
    if (channel_rate <= 0) {
        return fail(std::format("design_fine_taps: channel rate must be positive, got {}",
                                channel_rate));
    }
    if (half_width_hz <= 0 || 2 * half_width_hz >= channel_rate) {
        return fail(std::format("design_fine_taps: half-width {} Hz is outside (0, {}) for a "
                                "channel rate of {}",
                                half_width_hz, channel_rate / 2, channel_rate));
    }
    if (centre_denominator == 0) {
        return fail("design_fine_taps: filter centre denominator is zero");
    }
    if (!std::isfinite(attenuation_db) || attenuation_db < 0.0 || attenuation_db > 300.0) {
        return fail(std::format("design_fine_taps: stopband target {} dB is outside [0, 300]",
                                attenuation_db));
    }

    const auto phases = static_cast<std::size_t>(config.phases);
    const auto taps = static_cast<std::size_t>(config.taps);
    const std::size_t length = phases * taps;

    // The prototype, sampled at `phases` points per input sample, so it is an
    // ordinary lowpass at the interpolated rate and index i sits at
    // (i - centre)/phases input samples from its peak.
    const double centre = (static_cast<double>(length) - 1.0) / 2.0;
    const double cutoff =
        static_cast<double>(half_width_hz) / static_cast<double>(channel_rate);
    const double beta = kaiser_beta(attenuation_db);
    const double i0_beta = bessel_i0(beta);

    std::vector<double> prototype(length, 0.0);
    for (std::size_t i = 0; i < length; ++i) {
        const double position = (static_cast<double>(i) - centre) / static_cast<double>(phases);
        const double window = kaiser_window(i, length, beta, i0_beta);
        prototype[i] = 2.0 * cutoff * sinc_pi(2.0 * cutoff * position) * window;
    }

    // Normalise every branch to unit sum. This is what removes the
    // phase-dependent gain ripple that would otherwise appear as a tone at
    // the resampler's beat frequency, and after the modulation below it is
    // exactly unit gain at the filter's centre: a tone there comes out of any
    // branch at unit magnitude, because the branch's modulation factor and
    // the tone's own advance cancel term by term.
    for (std::size_t p = 0; p < phases; ++p) {
        double sum = 0.0;
        for (std::size_t k = 0; k < taps; ++k) {
            sum += prototype[p + k * phases];
        }
        if (std::abs(sum) < 1e-12) {
            continue;
        }
        for (std::size_t k = 0; k < taps; ++k) {
            prototype[p + k * phases] /= sum;
        }
    }

    std::vector<Complex32> table(length + 1U, Complex32{});

    // The modulation, referenced to the filter's own centre so the table is
    // conjugate-symmetric about its midpoint and the group delay carries no
    // extra phase term. The phase is reduced in integers, not by fmod on a
    // double: the numerator reaches 1.3e15 at the widest grid this project
    // accepts, which a double still holds exactly but with only three bits to
    // spare, and integers have no bits to spend.
    const std::int64_t period = 2 * absolute(centre_denominator) *
                                static_cast<std::int64_t>(phases) *
                                static_cast<std::int64_t>(channel_rate);
    const std::int64_t numerator =
        (centre_denominator < 0) ? -centre_numerator : centre_numerator;

    for (std::size_t i = 0; i < length; ++i) {
        // Twice the offset from the centre, so it stays an integer: the
        // centre of an even-length filter sits on a half sample.
        const auto twice_offset =
            2 * static_cast<std::int64_t>(i) - (static_cast<std::int64_t>(length) - 1);

        std::int64_t turns = (numerator * twice_offset) % period;
        if (turns < 0) {
            turns += period;
        }
        const double angle = kTwoPi * static_cast<double>(turns) / static_cast<double>(period);

        const double real = prototype[i] * std::cos(angle);
        const double imaginary = prototype[i] * std::sin(angle);
        table[i] = Complex32{static_cast<float>(real), static_cast<float>(imaginary)};
    }

    // The guard entry. Linear interpolation between branch p and branch p+1
    // reaches index phases*taps at the last branch of the last tap, which is
    // the prototype one whole input sample beyond its support.
    table[length] = Complex32{0.0F, 0.0F};

    return table;
}

Expected<std::vector<float>> design_audio_taps(std::uint32_t taps, SampleRate demod_rate,
                                               SampleRate audio_rate,
                                               double attenuation_db) {
    if (taps == 0 || taps > kMaxAudioTaps) {
        return fail(std::format("design_audio_taps: {} taps is outside [1, {}]", taps,
                                kMaxAudioTaps));
    }
    if (demod_rate <= 0 || audio_rate <= 0) {
        return fail(std::format("design_audio_taps: rates must be positive, got {} and {}",
                                demod_rate, audio_rate));
    }
    if (audio_rate > demod_rate) {
        return fail(std::format("design_audio_taps: audio rate {} is above the demodulation "
                                "rate {}, which is an interpolation rather than a decimation",
                                audio_rate, demod_rate));
    }

    // Cutoff at 0.45 of the audio rate, midway between a 0.4 passband edge
    // and the 0.5 that folds. Nothing in this project puts content above
    // 0.4*Fa: 19.2 kHz at 48 kHz is already above the FM broadcast audio
    // limit and far above any voice channel.
    const double cutoff = 0.45 * static_cast<double>(audio_rate) /
                          static_cast<double>(demod_rate);
    const double centre = (static_cast<double>(taps) - 1.0) / 2.0;
    const double beta = kaiser_beta(attenuation_db);
    const double i0_beta = bessel_i0(beta);

    std::vector<double> design(taps, 0.0);
    double sum = 0.0;
    for (std::uint32_t i = 0; i < taps; ++i) {
        const double position = static_cast<double>(i) - centre;
        const double window = kaiser_window(i, taps, beta, i0_beta);
        design[i] = 2.0 * cutoff * sinc_pi(2.0 * cutoff * position) * window;
        sum += design[i];
    }

    std::vector<float> result(taps, 0.0F);
    const double scale = (std::abs(sum) < 1e-12) ? 1.0 : 1.0 / sum;
    for (std::uint32_t i = 0; i < taps; ++i) {
        result[i] = static_cast<float>(design[i] * scale);
    }
    return result;
}

std::vector<float> design_dc_weights(std::uint32_t taps) {
    if (taps <= 1) {
        return std::vector<float>{1.0F};
    }

    // Hann rather than a boxcar. Both null DC exactly, because both sum to one
    // and the direct term the kernel subtracts them from is one, but a
    // boxcar's -13 dB first sidelobe leaves up to 1.9 dB of ripple in the
    // audio passband just above the corner where Hann's -31 dB leaves about
    // 0.25 dB. The endpoints are offset by one so neither weight is zero and
    // the effective length is the length asked for.
    std::vector<double> window(taps, 0.0);
    double sum = 0.0;
    for (std::uint32_t i = 0; i < taps; ++i) {
        const double phase =
            kTwoPi * static_cast<double>(i + 1U) / static_cast<double>(taps + 1U);
        window[i] = 0.5 - 0.5 * std::cos(phase);
        sum += window[i];
    }

    std::vector<float> result(taps, 0.0F);
    const double scale = (sum < 1e-12) ? 1.0 : 1.0 / sum;
    for (std::uint32_t i = 0; i < taps; ++i) {
        result[i] = static_cast<float>(window[i] * scale);
    }
    return result;
}

Expected<VrxPlan> plan_vrx(const GridParams& grid, SampleRate rate,
                           const engine::VrxParams& params,
                           const engine::VrxPlacement& placement) {
    if (const Status valid = validate(grid); !valid) {
        return std::unexpected(with_context(valid.error(), "plan_vrx grid"));
    }
    if (rate <= 0) {
        return fail(std::format("plan_vrx: sample rate must be positive, got {}", rate));
    }
    if (placement.channel_rate <= 0) {
        return fail("plan_vrx: placement carries no channel rate; call engine::place first");
    }
    if (placement.residual_denominator <= 0) {
        return fail("plan_vrx: placement carries a non-positive residual denominator");
    }

    VrxPlan plan;
    plan.placement = placement;
    plan.mode = static_cast<std::uint32_t>(params.demod);
    plan.channel_rate = placement.channel_rate;
    plan.audio_rate = (params.audio_rate > 0) ? params.audio_rate : kDefaultAudioRate;

    if (plan.mode > kDemodCw) {
        return fail(std::format("plan_vrx: demodulator {} is not one of the eight",
                                plan.mode));
    }
    if (params.bandwidth <= 0) {
        return fail(std::format("plan_vrx: bandwidth must be positive, got {} Hz",
                                params.bandwidth));
    }

    const Hertz widest = max_channel_bandwidth(placement);
    if (widest <= 0) {
        return fail(std::format(
            "plan_vrx: the residual of {}/{} Hz leaves nothing of a {} S/s channel for a "
            "receiver to use",
            placement.residual_numerator, placement.residual_denominator,
            placement.channel_rate));
    }
    plan.bandwidth = std::min(params.bandwidth, widest);
    plan.bandwidth_clamped = plan.bandwidth < params.bandwidth;

    const Hertz required =
        minimum_demod_rate(plan.mode, plan.bandwidth, std::max<Hertz>(0, params.cw_pitch));
    const auto decimation = static_cast<std::uint32_t>(
        std::max<std::int64_t>(1, (required + plan.audio_rate - 1) / plan.audio_rate));
    plan.demod_rate = static_cast<SampleRate>(decimation) * plan.audio_rate;

    // The fine filter has two jobs and the transition width is where they
    // meet.
    //
    // It is the anti-alias filter for the resampling, so it has to be in its
    // stopband before the fold: everything beyond Fd/2 from the filter's
    // centre lands back inside the output band, and nothing at all exists
    // beyond Fc/2, so whichever is nearer bounds the transition from above.
    // A transition centred on the cutoff reaches stopband at
    // bandwidth/2 + transition/2, which gives the limit below.
    //
    // It is also the receiver's own passband, which wants the transition as
    // narrow as the tap budget allows. Setting it by the anti-alias bound
    // alone is the mistake worth naming: at a 48 kHz demodulation rate that
    // bound is tens of kilohertz, the filter becomes a gentle hump with no
    // flat region at all, and the first thing to notice is that SSB stops
    // rejecting the opposite sideband. Measured at the loose bound, a 3 kHz
    // USB receiver rejected its unwanted sideband by 2.1 dB. Asking instead
    // for the stopband one half-bandwidth past the passband edge, so a signal
    // one whole bandwidth away is gone, takes the same measurement past 80 dB.
    const double stop_edge =
        0.5 * static_cast<double>(std::min(plan.demod_rate, plan.channel_rate));
    const double pass_edge = 0.5 * static_cast<double>(plan.bandwidth);

    const double transition_limit = 2.0 * (stop_edge - pass_edge);
    const double transition_wanted = pass_edge;
    const double transition = std::min(transition_limit, transition_wanted);
    if (!(transition > 0.0)) {
        return fail(std::format(
            "plan_vrx: a bandwidth of {} Hz leaves no transition band between its own edge "
            "and the {} Hz fold of a {} S/s channel resampled to {} S/s",
            plan.bandwidth, stop_edge, plan.channel_rate, plan.demod_rate));
    }

    plan.fine.phases = kDefaultPhases;
    plan.fine.nco_log2 = kDefaultNcoLog2;
    plan.fine.taps =
        std::clamp(kaiser_taps_for(kFineAttenuationDb, transition / static_cast<double>(
                                                           plan.channel_rate)),
                   8U, kMaxFineTaps);

    // What the tap count actually bought. Kaiser sets the stopband depth from
    // the window's shape parameter and the transition width from the length,
    // so a length that hit the cap widens the transition rather than raising
    // the sidelobes. Report the width that length implies, and report the
    // depth reached by the point the fold happens rather than the target, so
    // a receiver that could not afford its own filter says so.
    const double achieved_transition =
        (kFineAttenuationDb - 8.0) * static_cast<double>(plan.channel_rate) /
        (2.285 * kTwoPi * static_cast<double>(plan.fine.taps - 1U));
    plan.fine_transition_hz = achieved_transition;
    plan.fine_stopband_db =
        (achieved_transition <= transition_limit)
            ? -kFineAttenuationDb
            : -attenuation_reachable(plan.fine.taps,
                                     transition_limit / static_cast<double>(plan.channel_rate));
    plan.fine_group_delay_channel_samples =
        (static_cast<double>(plan.fine.phases) * static_cast<double>(plan.fine.taps) - 1.0) /
        (2.0 * static_cast<double>(plan.fine.phases));

    // Where the filter sits and what gets translated to DC. They differ for
    // three of the eight modes; see the table in core/shaders/vrx_fine.comp.
    const std::int64_t residual_numerator = placement.residual_numerator;
    const std::int64_t residual_denominator = placement.residual_denominator;

    plan.filter_numerator = residual_numerator;
    plan.filter_denominator = residual_denominator;
    plan.mix_numerator = residual_numerator;
    plan.mix_denominator = residual_denominator;

    if (plan.mode == kDemodUsb || plan.mode == kDemodLsb) {
        // Shift the passband to one side of the suppressed carrier. Over a
        // common denominator of 2*residual_denominator so the half-bandwidth
        // is exact even when the bandwidth is odd.
        const std::int64_t half = plan.bandwidth;  // over a denominator of 2
        plan.filter_numerator = 2 * residual_numerator +
                                (plan.mode == kDemodUsb ? 1 : -1) * residual_denominator * half;
        plan.filter_denominator = 2 * residual_denominator;
    }
    if (plan.mode == kDemodCw) {
        // Land the carrier on the operator's pitch rather than on DC, so it
        // is audible.
        plan.mix_numerator = residual_numerator - residual_denominator * params.cw_pitch;
        plan.mix_denominator = residual_denominator;
    }

    reduce_rational(plan.filter_numerator, plan.filter_denominator);
    reduce_rational(plan.mix_numerator, plan.mix_denominator);

    auto taps = design_fine_taps(plan.fine, plan.channel_rate, plan.bandwidth / 2,
                                 plan.filter_numerator, plan.filter_denominator,
                                 kFineAttenuationDb);
    if (!taps) {
        return std::unexpected(with_context(taps.error(), "plan_vrx fine taps"));
    }
    plan.fine_taps = std::move(*taps);

    auto delta = nco_delta(plan.mix_numerator, plan.mix_denominator, plan.demod_rate);
    if (!delta) {
        return std::unexpected(with_context(delta.error(), "plan_vrx mixer"));
    }
    plan.fine_nco_delta = *delta;

    // The demodulator.
    plan.demod.mode = plan.mode;

    // The raw tap is not a demodulator and does not land on the audio rate.
    // It hands out complex baseband at the receiver's bandwidth, so its
    // output rate is the demodulation rate: decimating it to 48 kHz would
    // throw away most of what it exists to expose. Everything else resamples
    // to the audio rate, by a whole factor after the detector.
    plan.demod.decimation = (plan.mode == kDemodRaw) ? 1U : decimation;
    plan.output_rate = (plan.mode == kDemodRaw) ? plan.demod_rate : plan.audio_rate;

    if (plan.demod.decimation == 1U) {
        plan.demod.audio_taps = 1U;
        plan.audio_stopband_db = 0.0;
    } else {
        // Transition from 0.4 to 0.5 of the audio rate, expressed at the
        // demodulation rate the filter actually runs at.
        const double audio_transition_fraction =
            0.1 * static_cast<double>(plan.audio_rate) / static_cast<double>(plan.demod_rate);
        std::uint32_t audio_taps = std::clamp(
            kaiser_taps_for(kAudioAttenuationDb, audio_transition_fraction), 3U, kMaxAudioTaps);
        // Odd, so the group delay is a whole number of samples and the filter
        // is linear phase with no half-sample bookkeeping downstream.
        if ((audio_taps % 2U) == 0U) {
            ++audio_taps;
        }
        plan.demod.audio_taps = std::min(audio_taps, kMaxAudioTaps);
        plan.audio_stopband_db =
            -attenuation_reachable(plan.demod.audio_taps, audio_transition_fraction);
    }
    plan.audio_group_delay_demod_samples =
        (static_cast<double>(plan.demod.audio_taps) - 1.0) / 2.0;

    if (plan.mode == kDemodAm) {
        const auto window = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(plan.demod_rate / kAmDcCornerHz, 16, kMaxDcTaps));
        plan.demod.dc_taps = window;
    } else {
        plan.demod.dc_taps = 1U;
    }

    auto audio_taps_table = design_audio_taps(plan.demod.audio_taps, plan.demod_rate,
                                              plan.audio_rate, kAudioAttenuationDb);
    if (!audio_taps_table) {
        return std::unexpected(with_context(audio_taps_table.error(), "plan_vrx audio taps"));
    }
    const std::vector<float> dc_weights = design_dc_weights(plan.demod.dc_taps);

    plan.demod_weights.reserve(audio_taps_table->size() + dc_weights.size());
    plan.demod_weights.insert(plan.demod_weights.end(), audio_taps_table->begin(),
                              audio_taps_table->end());
    plan.demod_weights.insert(plan.demod_weights.end(), dc_weights.begin(), dc_weights.end());

    plan.deviation = fm_deviation(plan.mode, plan.bandwidth);
    plan.demod_gain = vrx_demod_gain(plan.mode, plan.demod_rate, plan.deviation);

    return plan;
}

// ---------------------------------------------------------------------------
// Per-block parameters
// ---------------------------------------------------------------------------

Expected<VrxFineBlock> fine_block(const VrxPlan& plan, std::uint32_t channel_base,
                                  std::uint32_t channel_mask, std::uint32_t fine_mask,
                                  SampleIndex first_output, std::uint32_t count) {
    if (plan.channel_rate <= 0 || plan.demod_rate <= 0) {
        return fail("fine_block: the plan carries no rates");
    }
    if (count == 0) {
        return fail("fine_block: a dispatch of zero outputs has nothing to do");
    }

    const auto channel_rate = static_cast<SampleIndex>(plan.channel_rate);
    const auto demod_rate = static_cast<SampleIndex>(plan.demod_rate);

    const SampleIndex step_whole = channel_rate / demod_rate;
    const SampleIndex step_rem = channel_rate % demod_rate;

    // n_0 = floor(j_0 * Fc / Fd) and frac0 = (j_0 * Fc) mod Fd, computed
    // without ever forming j_0 * Fc. Splitting j_0 as a*Fd + b keeps every
    // product inside 64 bits for as long as n_0 itself does: b and step_rem
    // are both below Fd, so b*step_rem is below Fd squared.
    const SampleIndex whole = first_output / demod_rate;
    const SampleIndex part = first_output % demod_rate;
    const SampleIndex carry_product = part * step_rem;

    const SampleIndex first_input =
        first_output * step_whole + whole * step_rem + carry_product / demod_rate;
    const SampleIndex frac0 = carry_product % demod_rate;

    VrxFineBlock block;
    block.first_input = first_input;

    VrxFineParams& out = block.params;
    out.chan_base = channel_base;
    out.chan_mask = channel_mask;
    out.in_offset = static_cast<std::uint32_t>(first_input & static_cast<SampleIndex>(channel_mask));
    out.out_offset = static_cast<std::uint32_t>(first_output & static_cast<SampleIndex>(fine_mask));
    out.out_mask = fine_mask;
    out.count = count;
    out.step_whole = static_cast<std::uint32_t>(step_whole);
    out.step_rem = static_cast<std::uint32_t>(step_rem);
    out.out_rate = static_cast<std::uint32_t>(demod_rate);
    out.frac0 = static_cast<std::uint32_t>(frac0);
    out.inv_out_rate = 1.0F / static_cast<float>(demod_rate);

    // The phase at this block's first output, from the absolute index alone.
    // The uint64 product wraps modulo 2^64, which is the reduction modulo one
    // turn, so no block boundary can move it.
    const std::uint64_t phase = nco_phase(plan.fine_nco_delta, first_output);
    out.nco_phase_high = static_cast<std::uint32_t>(phase >> 32U);
    out.nco_phase_low = static_cast<std::uint32_t>(phase);
    out.nco_delta_high = static_cast<std::uint32_t>(plan.fine_nco_delta >> 32U);
    out.nco_delta_low = static_cast<std::uint32_t>(plan.fine_nco_delta);

    if (const Status valid = validate(plan.fine, out); !valid) {
        return std::unexpected(with_context(valid.error(), "fine_block"));
    }

    // The window this dispatch touches. The newest sample is the last
    // output's integer position and the oldest is taps-1 behind the first,
    // which is the filter's support.
    const SampleIndex last_accumulated =
        frac0 + static_cast<SampleIndex>(count - 1U) * step_rem;
    block.newest_input = first_input + static_cast<SampleIndex>(count - 1U) * step_whole +
                         last_accumulated / demod_rate;
    block.oldest_input = first_input - static_cast<SampleIndex>(plan.fine.taps - 1U);

    return block;
}

Expected<VrxDemodBlock> demod_block(const VrxPlan& plan, std::uint32_t fine_mask,
                                    SampleIndex first_audio, std::uint32_t count) {
    if (count == 0) {
        return fail("demod_block: a dispatch of zero outputs has nothing to do");
    }

    const auto decimation = static_cast<SampleIndex>(plan.demod.decimation);
    const SampleIndex first_fine = first_audio * decimation;

    VrxDemodBlock block;
    block.first_fine = first_fine;

    VrxDemodParams& out = block.params;
    out.in_mask = fine_mask;
    out.in_offset = static_cast<std::uint32_t>(first_fine & static_cast<SampleIndex>(fine_mask));
    out.count = count;
    out.gain = plan.demod_gain;

    if (const Status valid = validate(plan.demod, out); !valid) {
        return std::unexpected(with_context(valid.error(), "demod_block"));
    }

    // How far back the detector and the decimation filter reach. The AM
    // envelope's DC-removal window is much the longest of the three, which is
    // why it is worth naming rather than folding into a single worst case.
    const SampleIndex detector_history =
        (plan.demod.mode == kDemodAm)
            ? static_cast<SampleIndex>(plan.demod.dc_taps - 1U)
        : (plan.demod.mode == kDemodNfm || plan.demod.mode == kDemodWfm) ? SampleIndex{1}
                                                                         : SampleIndex{0};
    block.oldest_fine =
        first_fine - static_cast<SampleIndex>(plan.demod.audio_taps - 1U) - detector_history;

    return block;
}

}  // namespace revenant::dsp
