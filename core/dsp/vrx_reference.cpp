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
#include <limits>
#include <numbers>
#include <numeric>
#include <string>

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

// The top of the FM broadcast audio band, and the pilot that sits above it.
//
// 15 kHz is the ceiling core/dsp/synth/wfm_mod.cpp enforces on the transmit
// side, for the reason it gives there: the sum channel has to stay clear of
// the 19 kHz pilot, and the difference channel's upper sideband has to stay
// clear of the RDS subcarrier's lower edge at 54625 Hz.
//
// A WFM receiver's audio filter stops between them. Cutting at 0.45 of a
// 48 kHz audio rate instead, which is what every WFM receiver in this tree
// did until 2026-09-20, puts its passband edge at 19.2 kHz and passes the
// pilot: audible as a whine to anyone who can still hear it, and fatal to
// stereo, because the difference channel is recovered by multiplying the
// composite with a 38 kHz reference and the pilot lands from there straight
// on top of the programme.
constexpr double kFmAudioCeilingHz = 15'000.0;
constexpr double kFmPilotHz = 19'000.0;

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

// The passband translated from the receiver's frame into the frame the fine
// stage mixes to DC.
//
// One line and one mode, and it is the whole of what CW's pitch does to the
// geometry. The filter is centred on the tuned frequency for every mode, so
// a passband is always stated about VrxParams::center; the mix is a pitch
// BELOW that for CW so the carrier lands somewhere audible, which puts the
// tuned frequency a pitch above the mix centre and moves both edges with it.
//
// Everything that cares where the band sits relative to the fold goes
// through here: minimum_demod_rate, the transition-width bound in plan_vrx,
// and nothing else.
[[nodiscard]] Passband mix_frame(Passband band, std::uint32_t mode, Hertz cw_pitch) {
    if (mode != kDemodCw) {
        return band;
    }
    return Passband{band.low + cw_pitch, band.high + cw_pitch};
}

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

    const std::size_t wanted_taps = fine_tap_table_size(config);
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
    // WHAT THIS CHECK USED TO SAY, until 2026-09-22: every mode above
    // kDemodCw was refused as "is not one of the eight demodulators". There
    // were eleven, and the three it turned away are the digital voice modes.
    // See is_known_mode in the header for why the guard is a switch now.
    if (!is_known_mode(config.mode)) {
        return fail(std::format("vrx demod: mode {} is not an enumerator of engine::Demod",
                                config.mode));
    }
    const bool complex_output = is_complex_output(config.mode);
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
    if (complex_output && (config.decimation != 1U || config.audio_taps != 1U)) {
        return fail(std::format(
            "vrx demod: mode {} is a complex tap and hands out its baseband unchanged, so it "
            "cannot carry an audio decimation filter",
            config.mode));
    }
    if (config.channels != 1U && config.channels != 2U) {
        return fail(std::format(
            "vrx demod: {} channels is neither a mono detector nor an interleaved pair",
            config.channels));
    }
    if (complex_output && config.channels != 2U) {
        return fail(std::format(
            "vrx demod: mode {} is a complex tap and hands out a complex pair, so it is two "
            "channels and never one",
            config.mode));
    }
    if (config.pilot_taps > kMaxPilotTaps) {
        return fail(std::format("vrx demod: {} pilot taps is above the {} the kernel holds",
                                config.pilot_taps, kMaxPilotTaps));
    }
    if (config.pilot_taps != 0 && config.mode != kDemodWfm) {
        return fail(std::format(
            "vrx demod: a pilot bandpass belongs to FM stereo and mode {} is not wideband FM. "
            "The 19 kHz pilot and the 38 kHz difference channel exist only in a broadcast FM "
            "multiplex",
            config.mode));
    }
    if (config.pilot_taps != 0 && config.channels != 2U) {
        return fail("vrx demod: a pilot bandpass with one output channel decodes a stereo "
                    "difference channel and then throws it away");
    }
    if (config.channels == 2U && !complex_output && config.pilot_taps == 0) {
        return fail(std::format(
            "vrx demod: mode {} was asked for two channels with no pilot bandpass, so both "
            "would carry the same sum channel and the receiver would report stereo it never "
            "decoded",
            config.mode));
    }

    const std::uint64_t capacity = static_cast<std::uint64_t>(params.in_mask) + 1U;
    if ((capacity & static_cast<std::uint64_t>(params.in_mask)) != 0U) {
        return fail(std::format("vrx demod: input ring mask {} is not a power of two minus one",
                                params.in_mask));
    }

    // The span of fine samples one dispatch reads: the decimation stride over
    // the block, plus how far below its newest input one output reaches. A
    // ring shorter than that aliases two different samples onto one slot, and
    // the kernel would do the same thing bit for bit, so a diff would pass
    // while both sides read the wrong history.
    //
    // demod_fine_history is the whole of that reach and core/engine/
    // vrx_stage.cpp sizes the ring from the same function, so the refusal
    // here and the allocation there cannot disagree.
    const auto reach = static_cast<std::uint64_t>(demod_fine_history(config));
    const std::uint64_t span =
        (params.count == 0)
            ? 0U
            : (static_cast<std::uint64_t>(params.count - 1U) *
                   static_cast<std::uint64_t>(config.decimation) +
               reach + 1U);
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

    const std::size_t pilot_base =
        static_cast<std::size_t>(config.audio_taps) + static_cast<std::size_t>(config.dc_taps);
    const std::size_t rotation_base =
        pilot_base + 2U * static_cast<std::size_t>(config.pilot_taps);
    const std::size_t wanted_weights =
        (config.pilot_taps == 0)
            ? pilot_base
            : rotation_base + 2U * static_cast<std::size_t>(config.audio_taps);
    if (weights.size() != wanted_weights) {
        return fail(std::format(
            "reference_vrx_demod wants {} weights ({} audio, {} DC, {} pilot pairs, {} "
            "rotation pairs), got {}",
            wanted_weights, config.audio_taps, config.dc_taps, config.pilot_taps,
            (config.pilot_taps == 0) ? 0U : config.audio_taps, weights.size()));
    }

    const std::size_t capacity = static_cast<std::size_t>(params.in_mask) + 1U;
    if (fine_ring.size() < capacity) {
        return fail(std::format("reference_vrx_demod: fine ring holds {} samples, mask {} "
                                "implies a capacity of {}",
                                fine_ring.size(), params.in_mask, capacity));
    }

    const std::size_t wanted_audio =
        static_cast<std::size_t>(params.count) * static_cast<std::size_t>(config.channels);
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

    // The same figure the kernel holds, from the same decimal. See the
    // comment beside kPilotLockPower in core/shaders/vrx_demod.comp for
    // where the number comes from and for what a consumer sees when the
    // gate closes.
    constexpr float kPilotLockPower = 2.0e-5F;

    for (std::uint32_t i = 0; i < params.count; ++i) {
        const std::uint32_t index = params.in_offset + i * config.decimation;

        // The four complex taps, on the same branch because they are the same
        // arithmetic: the kernel's kComplexTap is true for exactly the modes
        // is_complex_output is, and both multiply by a gain the planner holds
        // at one.
        if (is_complex_output(config.mode)) {
            const Complex32 z = sample_at(index);
            audio[static_cast<std::size_t>(2U * i)] = z.real() * params.gain;
            audio[static_cast<std::size_t>(2U * i) + 1U] = z.imag() * params.gain;
            continue;
        }

        if (config.channels == 2U) {
            // FM stereo. Transcribes the kernel's branch operation for
            // operation: the same pilot accumulation order, the same
            // squaring rather than an arctangent, the same single detect()
            // feeding both paths, the same matrix at the end.
            const std::uint32_t pilot_at = index - (config.audio_taps / 2U);
            float pilot_real = 0.0F;
            float pilot_imag = 0.0F;
            for (std::uint32_t k = 0; k < config.pilot_taps; ++k) {
                const float composite = detect(pilot_at - k);
                const float term_real = composite * weights[pilot_base + 2U * k];
                const float term_imag = composite * weights[pilot_base + 2U * k + 1U];
                pilot_real = pilot_real + term_real;
                pilot_imag = pilot_imag + term_imag;
            }

            const float scaled_real = pilot_real * params.gain;
            const float scaled_imag = pilot_imag * params.gain;
            const float real_square = scaled_real * scaled_real;
            const float imag_square = scaled_imag * scaled_imag;
            const float pilot_power = real_square + imag_square;

            const float inverse = det_recip(std::max(pilot_power, kPilotLockPower));
            const float cross = scaled_real * scaled_imag;
            const float cos_two = (imag_square - real_square) * inverse;
            const float sin_two = (-2.0F * cross) * inverse;

            const float locked = (pilot_power >= kPilotLockPower) ? 1.0F : 0.0F;

            float sum = 0.0F;
            float difference = 0.0F;
            for (std::uint32_t t = 0; t < config.audio_taps; ++t) {
                const float weighted = detect(index - t) * weights[t];
                sum = sum + weighted;

                const float reference = sin_two * weights[rotation_base + 2U * t] -
                                        cos_two * weights[rotation_base + 2U * t + 1U];
                const float term = weighted * reference;
                difference = difference + term;
            }

            const float gated = difference * locked;
            audio[static_cast<std::size_t>(2U * i)] = (sum + gated) * params.gain;
            audio[static_cast<std::size_t>(2U * i) + 1U] = (sum - gated) * params.gain;
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

Passband default_passband(engine::Demod mode) {
    // No default label, for the reason spelled out on minimum_demod_rate
    // below: a ninth demodulator must stop here rather than inherit a
    // twelve-kilohertz window that suits nothing.
    switch (mode) {
        // The raw tap is not a demodulator and has no channel plan. Twelve
        // kilohertz is what VrxParams has always defaulted to and is kept so
        // that a caller who names no mode and no width gets what it used to.
        case engine::Demod::Raw: return Passband{-6'000, 6'000};

        // Broadcast AM's transmitted channel is 10 kHz wide.
        case engine::Demod::Am: return Passband{-5'000, 5'000};

        // Land mobile in a 25 kHz channel: 5 kHz deviation plus 3 kHz of
        // audio, twice, is 16 kHz of occupied bandwidth by Carson.
        case engine::Demod::Nfm: return Passband{-8'000, 8'000};

        // FM broadcast: 75 kHz deviation plus a 53 kHz baseband is 256 kHz
        // by Carson, and 200 kHz is the channel the band plan allocates.
        case engine::Demod::Wfm: return Passband{-100'000, 100'000};

        // This project's own SSB convention, and not a bandwidth centred on
        // anything. core/dsp/synth/modulators.h generates 300 to 3000 Hz of
        // audio, so a receiver anchored on the suppressed carrier at plus
        // 300 is sitting where the signal starts. 2700 rather than 3000 is
        // the ordinary communications receiver's upper edge.
        case engine::Demod::Usb: return Passband{300, 2'700};
        case engine::Demod::Lsb: return Passband{-2'700, -300};

        // Both sidebands of the same audio band.
        case engine::Demod::Dsb: return Passband{-3'000, 3'000};

        // 500 Hz about the carrier, which is the narrowest filter a general
        // coverage receiver ships and comfortably wider than a hand-sent
        // 25 wpm keying envelope. The sidetone is a mix and not an edge, so
        // it does not appear here.
        case engine::Demod::Cw: return Passband{-250, 250};

        // P25 Phase 1 FDMA occupies a 12.5 kHz channel. TIA-102.BAAA-A
        // clause 9.3 puts the transmit filter's stopband at 2880 Hz of
        // baseband, and the peak deviation is 1800 Hz (Table 9-1), so
        // Carson gives about 9.4 kHz of occupied bandwidth inside it. The
        // channel is the right window because the adjacent one is another
        // P25 carrier.
        case engine::Demod::P25p1: return Passband{-6'250, 6'250};

        // D-STAR DV is a 6.25 kHz channel. The JARL standard's system
        // specification table gives the occupied bandwidth as 6 kHz or less
        // and the carrier spacing as 6.25 kHz or more, so the occupied
        // bandwidth is the window and the spacing is the plan.
        case engine::Demod::Dstar: return Passband{-3'000, 3'000};

        // TETRA V+D is a 25 kHz channel carrying 18000 symbols per second
        // through a root raised cosine of roll-off 0.35 (EN 300 392-2
        // clauses 5.3 and 5.5), which occupies 18000 * 1.35 = 24.3 kHz.
        case engine::Demod::Tetra: return Passband{-12'500, 12'500};
    }

    // Not an enumerator. Nothing is known about the mode, so nothing is
    // known about its channel plan; the caller finds out by getting a band
    // it cannot use rather than one that looks plausible.
    return Passband{};
}

Expected<Passband> resolve_passband(const engine::VrxParams& params) {
    if (params.passband_low != 0 || params.passband_high != 0) {
        if (params.passband_low >= params.passband_high) {
            return fail(std::format(
                "resolve_passband: the passband runs from {} Hz to {} Hz, which is empty or "
                "inverted. Both edges are signed hertz from the receiver's centre and the low "
                "edge is the smaller number, so USB is 300 to 2700 and LSB is -2700 to -300",
                params.passband_low, params.passband_high));
        }
        return Passband{params.passband_low, params.passband_high};
    }

    if (params.bandwidth < 0) {
        return fail(std::format(
            "resolve_passband: bandwidth cannot be negative, got {} Hz, and no passband "
            "edges were given either",
            params.bandwidth));
    }

    // Nothing stated at all, so the mode's own channel plan answers. This is
    // what lets a client change mode without carrying a copy of the table:
    // it sends zeros and reads the granted edges back off VrxPlacement.
    // Zero and negative are deliberately different, because zero is "I did
    // not say" and a negative width is a mistake.
    if (params.bandwidth == 0) {
        const Passband fallback = default_passband(params.demod);
        if (fallback.width() <= 0) {
            return fail(std::format(
                "resolve_passband: demodulator {} has no default passband, so a request that "
                "states neither edges nor a bandwidth cannot be filled",
                static_cast<std::uint32_t>(params.demod)));
        }
        return fallback;
    }

    // The shorthand expansion. This is the geometry plan_vrx had before
    // edges existed, moved out of the planner and written down: symmetric
    // about the centre for every mode whose detector wants the carrier in
    // the middle, one-sided for the two sideband modes.
    //
    // A half-width is taken twice rather than once and subtracted, so an odd
    // bandwidth comes out symmetric and one hertz narrow instead of
    // asymmetric by one hertz. That is what the filter always was: the
    // planner passed bandwidth/2 to design_fine_taps as a half-width, so an
    // odd bandwidth already produced a filter of bandwidth - 1 and only the
    // reported figure differed.
    const Hertz half = params.bandwidth / 2;
    switch (params.demod) {
        case engine::Demod::Raw:
        case engine::Demod::Am:
        case engine::Demod::Nfm:
        case engine::Demod::Wfm:
        case engine::Demod::Dsb:
        case engine::Demod::Cw:

        // The three digital modes are symmetric about their carrier, like
        // the four above, because a linear modulation's spectrum is.
        case engine::Demod::P25p1:
        case engine::Demod::Dstar:
        case engine::Demod::Tetra: return Passband{-half, half};
        case engine::Demod::Usb: return Passband{0, params.bandwidth};
        case engine::Demod::Lsb: return Passband{-params.bandwidth, 0};
    }

    return fail(std::format("resolve_passband: demodulator {} is not one of the eleven",
                            static_cast<std::uint32_t>(params.demod)));
}

Passband clamp_to_channel(const engine::VrxPlacement& placement, Passband band) {
    const Hertz widest = max_channel_bandwidth(placement);
    if (widest <= 0) {
        // The channel can carry nothing at all: it has no rate, or the
        // receiver sits a whole half-channel or more off its centre. An
        // empty band at the receiver's own centre is the honest answer, and
        // it is what makes every caller's width test fire. Handing the
        // request straight back instead was how a receiver a channel cannot
        // carry came to be planned, with place() reporting it unclamped.
        return Passband{};
    }

    // ONE LIMIT, APPLIED TO EACH EDGE ON ITS OWN.
    //
    // The limit is max_channel_bandwidth halved, which is how far from the
    // receiver a symmetric band was allowed to reach before this change, and
    // it is not simply the distance to the channel's Nyquist. The difference
    // is the |residual| of headroom the symmetric figure reserved on the
    // slack side, and that headroom is load-bearing: plan_vrx bounds the
    // filter's transition by the distance from the fold to the passband
    // edge, and an edge granted all the way to the channel's Nyquist leaves
    // it zero, so a receiver too wide for its channel would be refused
    // outright where it used to be given the widest filter that fits.
    //
    // Keeping the limit symmetric about the receiver therefore costs a
    // clamped receiver up to 2*|residual| of band it could in principle have
    // had on one side, and buys every clamped receiver a filter that can
    // actually be designed. What is new is that the two edges are fitted
    // INDEPENDENTLY: a request too wide at the top keeps its lower edge
    // where the operator put it instead of losing the same amount at both
    // ends, which is what one width could only ever do.
    //
    // BOTH EDGES ARE FITTED INTO THE SAME INTERVAL, WHICH IS THE
    // POST-CONDITION. Each edge lands inside [-limit, +limit], so low <=
    // high always holds and the result is a band a caller can measure.
    // Clamping only the outside of each edge was not enough: a band lying
    // wholly above the channel, say [limit + 1000, limit + 2000], left its
    // low edge alone and pulled its high edge down to the limit, which
    // comes back INVERTED. Every caller then read a negative width and
    // reported a channel that had nothing left in it, when what had
    // happened was that the request was somewhere the channel does not
    // reach.
    const Hertz limit = widest / 2;

    Passband out;
    out.low = std::clamp(band.low, -limit, limit);
    out.high = std::clamp(band.high, -limit, limit);
    return out;
}

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

namespace {

// Refuses a request one channel cannot deliver, with the geometry in the
// message.
//
// Two failures, and they are different enough that reporting them the same
// way was how one of them went missing. The channel can carry nothing at
// all: max_channel_bandwidth is zero, which is where the guard plan_vrx
// used to have went when clamp_to_channel took over the fit. Or the channel
// carries something and the request is not inside it, either because it is
// wider than the limit on both sides or because it lies wholly past one of
// them.
//
// The message names WHERE THE REQUEST WAS against the limit, and only then
// the residual. Naming the residual first said the channel was full when
// the truth was usually that the receiver had been pointed outside it, and
// a residual over a denominator is not a number anyone reads a position off.
[[nodiscard]] Status channel_carries(const engine::VrxPlacement& placement, Passband band) {
    const Hertz widest = max_channel_bandwidth(placement);
    if (widest <= 0) {
        return fail(std::format(
            "one grid channel can carry nothing for a receiver placed here: a {} S/s channel "
            "with a residual of {}/{} Hz leaves no usable width at all, so the receiver is at "
            "least half a channel from the channel's own centre",
            placement.channel_rate, placement.residual_numerator,
            placement.residual_denominator));
    }

    if (clamp_to_channel(placement, band).width() > 0) {
        return {};
    }

    const Hertz limit = widest / 2;
    return fail(std::format(
        "a passband of {} to {} Hz has nothing inside the {} to {} Hz one grid channel can "
        "carry for a receiver placed here: a {} S/s channel with a residual of {}/{} Hz",
        band.low, band.high, -limit, limit, placement.channel_rate,
        placement.residual_numerator, placement.residual_denominator));
}

}  // namespace

Hertz fm_deviation(std::uint32_t mode, Hertz bandwidth) {
    if (bandwidth <= 0) {
        return 0;
    }

    // Over engine::Demod and with no default, for the reason spelled out on
    // minimum_demod_rate below: that is the shape /w14062 can see, so a ninth
    // demodulator stops here rather than being handed a deviation of zero.
    // Zero is right for six of the eight, which is what makes it dangerous to
    // give by omission. It does not stop at this function either:
    // vrx_demod_gain reads a zero deviation as "no discriminator" and returns
    // unity, so a ninth FM-family mode would emit phase advance in radians per
    // sample instead of the project's +/-1 convention.
    switch (static_cast<engine::Demod>(mode)) {
        case engine::Demod::Nfm:
            // Land mobile: 5 kHz peak deviation in a 25 kHz channel.
            return std::max<Hertz>(1, bandwidth / 5);
        case engine::Demod::Wfm:
            // FM broadcast: 75 kHz peak deviation in a 200 kHz channel.
            return std::max<Hertz>(1, (3 * bandwidth) / 8);
        case engine::Demod::Raw:
        case engine::Demod::Am:
        case engine::Demod::Usb:
        case engine::Demod::Lsb:
        case engine::Demod::Dsb:
        case engine::Demod::Cw:

        // Zero for the digital modes even though two of the three are
        // frequency modulations, and the reason is where the discriminator
        // sits rather than whether there is one. core/decode/dv_phy.cpp
        // discriminates after its own receive filter, which is where
        // TIA-102.BAAA-A clause 9.6 puts it, and the kernel hands these modes
        // out through its complex passthrough, which reads no deviation. A
        // deviation here would scale nothing and would read as a claim that
        // the kernel detects them.
        //
        // WHAT THIS PARAGRAPH USED TO SAY: "This value scales a kernel that
        // these modes never reach: engine::is_complex_tap routes them down
        // the raw tap". They reach it since 2026-09-22, as a passthrough.
        case engine::Demod::P25p1:
        case engine::Demod::Dstar:
        case engine::Demod::Tetra: return 0;
    }

    // Not an enumerator at all. No mode, so no channel plan, so no deviation.
    return 0;
}

Hertz minimum_demod_rate(std::uint32_t mode, Passband band_in_mix_frame) {
    const Hertz width = band_in_mix_frame.width();
    if (width <= 0) {
        return 0;
    }

    // The floor every mode shares: the fine filter's stopband edge is Fd/2
    // and its passband edge is half a width from its own centre, so Fd =
    // width would leave no transition band and therefore no filter. 1.5
    // widths leaves a transition of a quarter of a width. Measured from the
    // filter's centre and so independent of where the band sits.
    //
    // The shared floor used to be written (3*bandwidth + 1) / 2 against one
    // symmetric width, and this is the same expression against the width the
    // band actually has.
    const Hertz shape_floor = (3 * width + 1) / 2;

    // Whatever the detector does, the fine stream has to carry the band, and
    // the furthest edge from the mix centre is what sets that. This is the
    // term that used to be written out once for USB and LSB and once for CW;
    // see the header for why those two cases are gone and what they were.
    const Hertz reach = band_in_mix_frame.reach_from(0);
    const Hertz floor_rate = std::max(shape_floor, 2 * reach);

    // THE PARAMETER IS THE 32-BIT WORD, THE SWITCH IS OVER THE ENUM.
    //
    // The uint32 is deliberate and stays. The mode reaches the device as a
    // specialization constant: VrxDemodConfig::mode mirrors
    // core/shaders/vrx_demod.comp's `layout(constant_id = 1) const uint
    // kMode`, core/engine/vrx_stage.cpp packs it into a std::uint32_t array
    // with the other three, and VrxPlan::mode is the copy it is packed from.
    // engine::Demod has an 8-bit underlying type, so the widened word is what
    // the planner and the stage are actually holding, and it is what this
    // function, fm_deviation, vrx_demod_gain and the stage's
    // detector_history() are all handed.
    //
    // Switching over the enum instead costs one cast and buys the diagnostic.
    // C4062 fires only on an enum switch with no default label, so over a
    // uint32 with a default there was nothing for it to see. The
    // static_asserts beside the kDemod constants pin each enumerator to its
    // value, which catches a REORDER; this catches an ADDITION, which is the
    // case they are blind to. Getting a build error here is the point: a new
    // demodulator cannot be given a rate by omission.
    switch (static_cast<engine::Demod>(mode)) {
        case engine::Demod::Am:
            // The envelope of a band of width W carries content out to W,
            // wherever that band sits, because an envelope is built from
            // differences within the band and not from the band's position.
            // This is the one mode the reach term above does not cover.
            return std::max(floor_rate, 2 * width);
        case engine::Demod::Usb:
        case engine::Demod::Lsb:
            // A product detector produces audio out to the furthest edge
            // from the mix centre, which is the reach term already in
            // floor_rate. Spelled out rather than merged with the four
            // below, because arriving at the same number for a different
            // reason is not the same claim.
            return floor_rate;
        case engine::Demod::Cw:
            // The carrier is translated to the pitch, so the pitch is
            // already inside the band the caller handed over and the reach
            // term carries it. Same expression as USB and LSB and the same
            // number it always produced.
            return floor_rate;
        case engine::Demod::Raw:
        case engine::Demod::Nfm:
        case engine::Demod::Wfm:
        case engine::Demod::Dsb:
            // A discriminator produces the modulating audio, which is
            // narrower than the channel, and DSB's real part folds a
            // symmetric band onto half its width. Raw is not detected at all.
            // Spelled out rather than left to a default, because sitting at
            // the shared floor is a claim about these four detectors and not
            // a fallback.
            return floor_rate;

        // The three digital modes are complex taps, so nothing detects them
        // here and the reach term would be the whole story, except that the
        // thing on the other end of the tap has a floor of its own. Each
        // decoder in core/decode refuses fewer than two samples per symbol,
        // and this states that floor so a plan below it fails here, naming
        // the rate, rather than at the decoder's create().
        //
        // It is a floor and not the rate these modes run at.
        // demod_rate_for rounds them up in steps of complex_tap_rate_step,
        // which is ten or four samples per symbol, so a plan that reaches
        // the decoder is always well above it.
        //
        // WHAT THIS PARAGRAPH USED TO SAY: "The timing recovery in
        // core/decode/dv_phy.cpp is a Gardner detector, which needs a sample
        // halfway between symbol instants to look at". core/decode/dv_phy.h
        // records that a Gardner loop was written first and did not converge
        // on C4FM's four levels, and what shipped is a feedforward square-law
        // estimator over a Farrow interpolator. The two-sample floor is each
        // decoder's own check in its create(), whatever the estimator.
        case engine::Demod::P25p1:
            // TIA-102.BAAA-A clause 9.2: 4800 symbols per second.
            return std::max<Hertz>(floor_rate, 9'600);
        case engine::Demod::Dstar:
            // JARL Ver 7.0 clause 4.1.2 b: 96 bits every 20 ms, so 4800.
            return std::max<Hertz>(floor_rate, 9'600);
        case engine::Demod::Tetra:
            // EN 300 392-2 clause 5.3: 36 kbit/s at two bits per symbol.
            return std::max<Hertz>(floor_rate, 36'000);
    }

    // Not an enumerator. Unreachable from plan_vrx, which range-checks the
    // mode before it gets here, so this is a direct caller with a bad value.
    //
    // The floor is the wrong thing to hand it. The shape floor is the rate
    // below which no mode can be filtered at all, not a rate at which any
    // given mode works, and a detector that folds the spectrum given it
    // produces audio out past Fd/2 that comes back inside the band, quietly,
    // with nothing downstream measuring it. The widest requirement any mode
    // makes costs a bigger filter and a faster resampler, which is visible
    // and recoverable.
    return std::max(floor_rate, 2 * width);
}

Hertz minimum_demod_rate(std::uint32_t mode, Hertz bandwidth, Hertz cw_pitch) {
    if (bandwidth <= 0) {
        return 0;
    }

    // Clamped here for the same reason plan_vrx and demod_rate_for clamp
    // it: a pitch is a distance below the tuned frequency, so a negative
    // one translated literally moves the band the wrong way and asks for a
    // rate the engine will never run. See the header for the figures.
    const Hertz pitch = std::max<Hertz>(0, cw_pitch);

    engine::VrxParams shorthand;
    shorthand.bandwidth = bandwidth;
    shorthand.cw_pitch = pitch;
    if (is_known_mode(mode)) {
        shorthand.demod = static_cast<engine::Demod>(mode);
    }

    auto band = resolve_passband(shorthand);
    if (!band) {
        return 0;
    }
    return minimum_demod_rate(mode, mix_frame(*band, mode, pitch));
}

Expected<SampleRate> demod_rate_for(const engine::VrxParams& params,
                                    const engine::VrxPlacement& placement,
                                    SampleRate audio_rate) {
    if (audio_rate <= 0) {
        return fail(std::format("demod_rate_for: audio rate must be positive, got {}",
                                audio_rate));
    }

    const auto mode = static_cast<std::uint32_t>(params.demod);
    if (!is_known_mode(mode)) {
        return fail(std::format("demod_rate_for: demodulator {} is not an enumerator of "
                                "engine::Demod",
                                mode));
    }

    auto band = resolve_passband(params);
    if (!band) {
        return std::unexpected(with_context(band.error(), "demod_rate_for"));
    }
    if (const Status fits = channel_carries(placement, *band); !fits) {
        return std::unexpected(with_context(fits.error(), "demod_rate_for"));
    }
    const Passband granted = clamp_to_channel(placement, *band);

    const Hertz required =
        minimum_demod_rate(mode, mix_frame(granted, mode, std::max<Hertz>(0, params.cw_pitch)));

    // A whole multiple of the audio rate for every mode that ends in audio,
    // so the decimation is an integer. The digital voice modes end in a
    // decoder instead, and step in the rate that decoder was measured at.
    const SampleRate tap_step = complex_tap_rate_step(mode);
    const SampleRate step = (tap_step > 0) ? tap_step : audio_rate;
    const auto multiple = std::max<std::int64_t>(1, (required + step - 1) / step);
    return static_cast<SampleRate>(multiple * step);
}

SampleRate complex_tap_rate_step(std::uint32_t mode) {
    if (!is_known_mode(mode)) {
        return 0;
    }
    // Over the enum with no default, for the reason the three functions
    // around it give: a twelfth mode has to say whether it ends in a decoder.
    switch (static_cast<engine::Demod>(mode)) {
        // Ten samples per symbol of TIA-102.BAAA-A clause 9.2's 4800.
        case engine::Demod::P25p1: return 48'000;

        // Ten samples per bit of JARL Ver 7.0 clause 4.1.2 b's 4800.
        case engine::Demod::Dstar: return 48'000;

        // Four samples per symbol of EN 300 392-2 clause 5.3's 18000.
        case engine::Demod::Tetra: return 72'000;

        // The raw tap is a complex tap too and is not here. It has no
        // decoder behind it to size a rate for, and it has always stepped in
        // the audio rate; a caller wanting it at some other rate names that
        // rate as the audio rate.
        case engine::Demod::Raw:
        case engine::Demod::Am:
        case engine::Demod::Nfm:
        case engine::Demod::Wfm:
        case engine::Demod::Usb:
        case engine::Demod::Lsb:
        case engine::Demod::Dsb:
        case engine::Demod::Cw: return 0;
    }
    return 0;
}

float vrx_demod_gain(std::uint32_t mode, SampleRate demod_rate, Hertz deviation) {
    // The same exhaustive-switch guard as its two neighbours above, for the
    // same reason: unity is correct for nine of the eleven modes and is
    // therefore what a twelfth would silently inherit. The two discriminator
    // cases break out to the scaling below; a value that is no enumerator at
    // all falls through with them and is caught by the deviation test, since
    // fm_deviation returns zero for it.
    switch (static_cast<engine::Demod>(mode)) {
        case engine::Demod::Nfm:
        case engine::Demod::Wfm: break;
        case engine::Demod::Raw:
        case engine::Demod::Am:
        case engine::Demod::Usb:
        case engine::Demod::Lsb:
        case engine::Demod::Dsb:
        case engine::Demod::Cw:

        // Unity for the digital modes, on the same reasoning as Raw: they
        // are taps and the samples they hand out are the fine stage's, at
        // the channel's own level. The kernel's complex passthrough is the
        // one place this number is applied to them, and a gain of exactly
        // one is what makes that passthrough an identity to the bit. Scaling them would put a constant between
        // core/decode's slicers and the deviation figures their standards
        // state, which is the one thing those slicers are entitled to
        // assume about their input.
        case engine::Demod::P25p1:
        case engine::Demod::Dstar:
        case engine::Demod::Tetra:
            // Raw is a passthrough, AM's envelope is already in the same
            // units as the input, and a product detector's real part is too.
            // Unity keeps every mode on the one convention: a unit-amplitude
            // signal fully modulating its own mode swings the audio to +/-1.
            //
            // DSB is the one mode where that is not the whole story, and the
            // reason is physics rather than a missing constant. Its complex
            // envelope is real and in phase with a carrier that is not being
            // transmitted, so a product detector with no carrier recovery
            // scales the audio by the cosine of the residual phase between
            // the receiver's oscillator and that suppressed carrier. The
            // measured figure at one tuning is 0.743 against a residual phase
            // of -0.733 radian, which is exactly its cosine. USB, LSB and CW
            // are not affected: their audio is a rotating phasor, so a
            // constant phase offset moves its phase and not its amplitude.
            // Closing it needs a loop with carried state, which belongs above
            // a kernel.
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

    // Through fine_tap_table_size rather than length + 1, because that
    // function is what VrxShape cites when it leaves the table's length out
    // of the shape. A designer that allocated its own arithmetic is how the
    // two would come apart.
    std::vector<Complex32> table(fine_tap_table_size(config), Complex32{});

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
                                               double attenuation_db, double cutoff_hz) {
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

    if (cutoff_hz < 0.0 || cutoff_hz >= 0.5 * static_cast<double>(audio_rate)) {
        if (cutoff_hz != 0.0) {
            return fail(std::format(
                "design_audio_taps: a cutoff of {} Hz is outside (0, {}), which is what a "
                "{} S/s audio stream can represent. Pass zero to take 0.45 of the rate",
                cutoff_hz, 0.5 * static_cast<double>(audio_rate), audio_rate));
        }
    }

    // Cutoff at 0.45 of the audio rate unless the caller named one, midway
    // between a 0.4 passband edge and the 0.5 that folds.
    //
    // WHAT THIS COMMENT USED TO CLAIM. Until 2026-09-20 it read "Nothing in
    // this project puts content above 0.4*Fa: 19.2 kHz at 48 kHz is already
    // above the FM broadcast audio limit and far above any voice channel."
    // The first half is true of the programme audio and false of what a WFM
    // discriminator hands this filter, which is the whole multiplex: the
    // 19 kHz pilot sits below 19.2 kHz, inside the passband, and a stereo
    // decoder folds it straight onto the difference channel. The claim was
    // right about the audio and was being made about the composite.
    const double cutoff = (cutoff_hz > 0.0)
                              ? cutoff_hz / static_cast<double>(demod_rate)
                              : 0.45 * static_cast<double>(audio_rate) /
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

Expected<DeemphasisDesign> design_deemphasis_taps(engine::Deemphasis curve,
                                                  SampleRate audio_rate) {
    DeemphasisDesign design;

    const double tau = engine::deemphasis_seconds(curve);
    if (tau <= 0.0) {
        // The identity. One unit tap, so a caller can fold unconditionally
        // and get its own filter back rather than branching.
        design.taps = std::vector<float>{1.0F};
        design.truncation_db = std::numeric_limits<double>::infinity();
        return design;
    }
    if (audio_rate <= 0) {
        return fail(std::format("design_deemphasis_taps: audio rate must be positive, got {}",
                                audio_rate));
    }

    // a = exp(-T/tau), the impulse-invariant pole of 1/(1 + s*tau). The
    // bilinear transform is the other standard mapping and would warp the
    // corner; at 2122 Hz against a 24 kHz Nyquist the warp is under a
    // percent, so this is a choice between two defensible answers rather
    // than between right and wrong, and the exponential is the one whose
    // impulse response is exactly the geometric series being truncated here.
    const double period = 1.0 / static_cast<double>(audio_rate);
    const double pole = std::exp(-period / tau);
    design.pole = pole;

    // The point where a^k is below the gap between 1.0 and the next float,
    // so a longer expansion cannot change any output bit. 2^-24 rather than
    // 2^-23 because the terms are summed and the accumulated tail is up to
    // a^N/(1-a) rather than a^N.
    constexpr double kFloatFloor = 1.0 / 16'777'216.0;
    const double exact_length = std::log(kFloatFloor * (1.0 - pole)) / std::log(pole);
    auto length = static_cast<std::uint32_t>(std::ceil(std::max(1.0, exact_length))) + 1U;
    if (length > kMaxDeemphasisTaps) {
        length = kMaxDeemphasisTaps;
        design.truncated_early = true;
    }

    std::vector<double> response(length, 0.0);
    double sum = 0.0;
    double weighted = 0.0;
    double term = 1.0;
    for (std::uint32_t k = 0; k < length; ++k) {
        response[k] = term;
        sum += term;
        weighted += static_cast<double>(k) * term;
        term *= pole;
    }

    // Renormalised to sum to exactly one rather than scaled by (1-a). Both
    // give unity at DC for an untruncated series and only this one does for
    // a truncated one, and a DC gain that is 1 - a^N instead of 1 is a
    // level error the operator would hear as the curve being slightly quiet.
    const double scale = 1.0 / sum;
    design.taps.resize(length);
    for (std::uint32_t k = 0; k < length; ++k) {
        design.taps[k] = static_cast<float>(response[k] * scale);
    }

    // term is a^length by now, which is the fraction of the direct term the
    // expansion threw away.
    design.truncation_db = (term > 0.0) ? -20.0 * std::log10(term)
                                        : std::numeric_limits<double>::infinity();
    design.group_delay_audio_samples = weighted / sum;
    return design;
}

Expected<std::vector<float>> fold_deemphasis(ConstRealSpan decimation_taps,
                                             ConstRealSpan deemphasis_taps,
                                             std::uint32_t decimation) {
    if (decimation_taps.empty() || deemphasis_taps.empty()) {
        return fail("fold_deemphasis: both filters need at least one tap");
    }
    if (decimation == 0) {
        return fail("fold_deemphasis: decimation is zero");
    }

    // A single unit tap is the identity whichever side it is on, and
    // returning the other side untouched keeps a flat curve bit-identical
    // to the table this tree designed before de-emphasis existed. That is
    // what makes "the curve is off" and "there is no curve" the same plan
    // rather than two that differ in the last place.
    if (deemphasis_taps.size() == 1 && deemphasis_taps[0] == 1.0F) {
        return std::vector<float>(decimation_taps.begin(), decimation_taps.end());
    }

    const std::size_t stride = static_cast<std::size_t>(decimation);
    const std::size_t length =
        decimation_taps.size() + (deemphasis_taps.size() - 1U) * stride;
    if (length > kMaxAudioTaps) {
        return fail(std::format(
            "fold_deemphasis: a {}-tap decimation filter and a {}-tap de-emphasis curve at a "
            "decimation of {} fold to {} taps, and the kernel's audio filter holds {}",
            decimation_taps.size(), deemphasis_taps.size(), decimation, length,
            kMaxAudioTaps));
    }

    // In double and rounded once, which is the same discipline every other
    // table in this file is built with. Nothing here is a twin: both the
    // kernel and reference_vrx_demod read the floats this produces, so the
    // arithmetic that made them has no operation order to match.
    std::vector<double> folded(length, 0.0);
    for (std::size_t k = 0; k < deemphasis_taps.size(); ++k) {
        const double weight = static_cast<double>(deemphasis_taps[k]);
        const std::size_t offset = k * stride;
        for (std::size_t j = 0; j < decimation_taps.size(); ++j) {
            folded[offset + j] += weight * static_cast<double>(decimation_taps[j]);
        }
    }

    std::vector<float> result(length, 0.0F);
    for (std::size_t i = 0; i < length; ++i) {
        result[i] = static_cast<float>(folded[i]);
    }
    return result;
}

std::uint32_t stereo_pilot_taps(SampleRate demod_rate) {
    if (demod_rate <= 0) {
        return 0;
    }
    const double transition = kStereoPilotTransitionHz / static_cast<double>(demod_rate);
    std::uint32_t taps =
        std::clamp(kaiser_taps_for(kStereoPilotAttenuationDb, transition), 16U, kMaxPilotTaps);
    // Odd, so the prototype is symmetric about a whole sample. Nothing in
    // the kernel corrects a group delay, for the reason design_stereo_tables
    // gives, but an even-length window has no centre tap and its two halves
    // are not mirror images, which costs the prototype its linear phase in
    // the bands it is there to reject.
    if ((taps % 2U) == 0U) {
        ++taps;
    }
    return std::min(taps, kMaxPilotTaps);
}

Expected<StereoDesign> design_stereo_tables(SampleRate demod_rate, std::uint32_t audio_taps,
                                            std::uint32_t pilot_taps) {
    if (demod_rate <= 0) {
        return fail(std::format("design_stereo_tables: demodulation rate must be positive, "
                                "got {}",
                                demod_rate));
    }
    if (pilot_taps == 0 || pilot_taps > kMaxPilotTaps) {
        return fail(std::format("design_stereo_tables: {} pilot taps is outside [1, {}]",
                                pilot_taps, kMaxPilotTaps));
    }
    if (audio_taps == 0 || audio_taps > kMaxAudioTaps) {
        return fail(std::format("design_stereo_tables: {} audio taps is outside [1, {}]",
                                audio_taps, kMaxAudioTaps));
    }
    if (2 * kStereoPilotHz >= demod_rate) {
        return fail(std::format(
            "design_stereo_tables: a {} S/s demodulation rate cannot represent the {} Hz "
            "stereo subcarrier, which is twice the {} Hz pilot",
            demod_rate, 2 * kStereoPilotHz, kStereoPilotHz));
    }

    StereoDesign design;

    const double rate = static_cast<double>(demod_rate);
    const double cutoff = kStereoPilotHalfWidthHz / rate;
    const double centre = (static_cast<double>(pilot_taps) - 1.0) / 2.0;
    const double beta = kaiser_beta(kStereoPilotAttenuationDb);
    const double i0_beta = bessel_i0(beta);

    std::vector<double> prototype(pilot_taps, 0.0);
    double sum = 0.0;
    for (std::uint32_t k = 0; k < pilot_taps; ++k) {
        const double position = static_cast<double>(k) - centre;
        const double window = kaiser_window(k, pilot_taps, beta, i0_beta);
        prototype[k] = 2.0 * cutoff * sinc_pi(2.0 * cutoff * position) * window;
        sum += prototype[k];
    }
    const double scale = (std::abs(sum) < 1e-12) ? 1.0 : 1.0 / sum;

    // Modulated by exp(+j*omega*k) against the SAME k the convolution
    // indexes with, which is what makes the filter delayless at its own
    // centre frequency: the tone's advance over the tap and the tap's own
    // modulation cancel term by term, leaving the prototype's DC gain and a
    // quarter turn. That is why nothing in the kernel corrects a group
    // delay and why the pilot phase it measures is the phase at the instant
    // it asked about rather than half a window earlier.
    const double pilot_turns = static_cast<double>(kStereoPilotHz) / rate;
    design.pilot.resize(2U * static_cast<std::size_t>(pilot_taps));
    for (std::uint32_t k = 0; k < pilot_taps; ++k) {
        const double turns = std::fmod(pilot_turns * static_cast<double>(k), 1.0);
        const double angle = kTwoPi * turns;
        const double weight = prototype[k] * scale;
        design.pilot[2U * k] = static_cast<float>(weight * std::cos(angle));
        design.pilot[2U * k + 1U] = static_cast<float>(weight * std::sin(angle));
    }

    // The reference is measured once, at the audio filter's centre tap, and
    // carried to every other tap by this table: entry t is twice the cosine
    // and twice the sine of the subcarrier's advance from that centre. The
    // factor of two is the difference channel's own, since recovering a
    // double-sideband suppressed-carrier signal is a product with twice the
    // reference, folded in here so the kernel's inner loop is one
    // multiply-add rather than two.
    const double subcarrier_turns = 2.0 * pilot_turns;
    const auto pivot = static_cast<std::int64_t>(audio_taps / 2U);
    design.rotation.resize(2U * static_cast<std::size_t>(audio_taps));
    for (std::uint32_t t = 0; t < audio_taps; ++t) {
        const auto offset = static_cast<double>(static_cast<std::int64_t>(t) - pivot);
        const double turns = std::fmod(subcarrier_turns * offset, 1.0);
        const double angle = kTwoPi * turns;
        design.rotation[2U * t] = static_cast<float>(2.0 * std::cos(angle));
        design.rotation[2U * t + 1U] = static_cast<float>(2.0 * std::sin(angle));
    }

    design.pilot_transition_hz = (kStereoPilotAttenuationDb - 8.0) * rate /
                                 (2.285 * kTwoPi * static_cast<double>(pilot_taps - 1U));
    design.pilot_stopband_db =
        -attenuation_reachable(pilot_taps, kStereoPilotTransitionHz / rate);
    return design;
}

namespace {

// Everything plan_vrx derives that is not one of the three filter tables.
//
// The split exists so vrx_shape_for can answer "is this a rebuild" without
// designing a Kaiser-windowed sinc per polyphase branch. It returns a VrxPlan
// rather than a smaller struct because the tables are the only fields it
// leaves empty and because a second struct carrying half the plan's fields is
// the kind of parallel list this whole change is removing.
//
// Errors are still reported as plan_vrx's, since both public entry points
// are the same arithmetic and a caller reading "vrx_shape_for" in one message
// and "plan_vrx" in another for the identical refusal learns nothing from the
// difference.
[[nodiscard]] Expected<VrxPlan> design_vrx(const GridParams& grid, SampleRate rate,
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

    // WHAT THIS CHECK USED TO SAY, until 2026-09-22: any mode above
    // kDemodCw was "not one of the eight". That was every digital voice
    // mode, so none of them could be planned, and vrx_shape_for, which is
    // this function, refused every retune of one on the control plane.
    if (!is_known_mode(plan.mode)) {
        return fail(std::format("plan_vrx: demodulator {} is not an enumerator of "
                                "engine::Demod",
                                plan.mode));
    }
    const bool complex_output = is_complex_output(plan.mode);

    // The request resolved into two edges, once, here. resolve_passband is
    // the only place a shorthand is ever expanded and place() calls the same
    // function, so the placement's granted edges and the plan's cannot
    // disagree about what was asked for.
    auto requested = resolve_passband(params);
    if (!requested) {
        return std::unexpected(with_context(requested.error(), "plan_vrx"));
    }

    if (const Status fits = channel_carries(placement, *requested); !fits) {
        return std::unexpected(with_context(fits.error(), "plan_vrx"));
    }

    plan.passband = clamp_to_channel(placement, *requested);
    plan.bandwidth = plan.passband.width();
    plan.bandwidth_clamped = plan.passband != *requested;

    const Hertz cw_pitch = std::max<Hertz>(0, params.cw_pitch);
    const Passband mixed = mix_frame(plan.passband, plan.mode, cw_pitch);

    // Through demod_rate_for rather than inline, so the figure a caller can
    // get cheaply and the figure this planner builds a filter for are the
    // same arithmetic rather than two copies of it. Graph::set_vrx_params
    // asks the cheap one on every retune.
    auto resolved_rate = demod_rate_for(params, placement, plan.audio_rate);
    if (!resolved_rate) {
        return std::unexpected(with_context(resolved_rate.error(), "plan_vrx"));
    }
    plan.demod_rate = *resolved_rate;
    const auto decimation =
        static_cast<std::uint32_t>(plan.demod_rate / plan.audio_rate);

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
    // TWO EDGES AND SO TWO BOUNDS, WHERE THERE USED TO BE ONE.
    //
    // The fold is at plus and minus stop_edge about the MIX centre, so an
    // asymmetric passband is nearer one of them than the other and the
    // nearer one decides. Written as one min over the two distances. A
    // symmetric band puts both distances at stop_edge - width/2 and this
    // reduces to the 2*(stop_edge - pass_edge) it replaces, to the bit.
    //
    // The approximation this does NOT fix, and which was already here:
    // stop_edge takes the channel's limit as Fc/2 from the MIX centre, and
    // the channel's band is Fc/2 from the CHANNEL centre, which is
    // |residual| away. So when the channel rather than the fold is the
    // binding constraint this is optimistic by up to |residual| on one side.
    // Left alone deliberately: correcting it changes the tap count of every
    // receiver whose demodulation rate exceeds its channel rate, which is a
    // measurement this change is not making.
    const double stop_edge =
        0.5 * static_cast<double>(std::min(plan.demod_rate, plan.channel_rate));
    const double transition_limit =
        2.0 * std::min(stop_edge - static_cast<double>(mixed.high),
                       stop_edge + static_cast<double>(mixed.low));
    const double transition_wanted = 0.5 * static_cast<double>(plan.bandwidth);
    const double transition = std::min(transition_limit, transition_wanted);
    if (!(transition > 0.0)) {
        return fail(std::format(
            "plan_vrx: a passband of {} to {} Hz leaves no transition band between its own "
            "edge and the {} Hz fold of a {} S/s channel resampled to {} S/s",
            plan.passband.low, plan.passband.high, stop_edge, plan.channel_rate,
            plan.demod_rate));
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

    // Where the filter sits and what gets translated to DC.
    //
    // ONE RULE FOR ALL EIGHT MODES, WHERE THERE USED TO BE THREE CASES. The
    // filter is centred on the middle of the granted passband and the mix is
    // the receiver's own centre, except for CW, which mixes a pitch below so
    // the carrier is audible.
    //
    // WHAT THIS BLOCK USED TO DO. Until this change it read a single
    // symmetric bandwidth and derived the USB and LSB centres from it:
    // filter = residual +/- B/2, over a denominator of 2*residual_denominator
    // so an odd bandwidth stayed exact. That is the same arithmetic as the
    // line below for the bands the shorthand expands USB and LSB to, [0, B]
    // and [-B, 0], whose midpoints are +B/2 and -B/2, so nothing about those
    // two modes moves by a hertz. It is recorded rather than removed because
    // core/shaders/vrx_fine.comp documents the four-row table it produced and
    // a reader will arrive from there looking for it.
    //
    // Over a common denominator of 2*residual_denominator for the same
    // reason it always was: the midpoint of two integer edges lands on a
    // half hertz whenever their sum is odd, and rounding it is a tuning
    // offset nobody can source.
    const std::int64_t residual_numerator = placement.residual_numerator;
    const std::int64_t residual_denominator = placement.residual_denominator;

    plan.filter_numerator =
        2 * residual_numerator + residual_denominator * (plan.passband.low + plan.passband.high);
    plan.filter_denominator = 2 * residual_denominator;

    plan.mix_numerator = residual_numerator;
    plan.mix_denominator = residual_denominator;
    if (plan.mode == kDemodCw) {
        // Land the carrier on the operator's pitch rather than on DC, so it
        // is audible.
        plan.mix_numerator = residual_numerator - residual_denominator * cw_pitch;
        plan.mix_denominator = residual_denominator;
    }

    reduce_rational(plan.filter_numerator, plan.filter_denominator);
    reduce_rational(plan.mix_numerator, plan.mix_denominator);

    // The demodulator.
    plan.demod.mode = plan.mode;

    // The complex taps are not demodulators and do not land on the audio
    // rate. They hand out complex baseband at the receiver's bandwidth, so
    // their output rate is the demodulation rate: decimating the raw tap to
    // 48 kHz would throw away most of what it exists to expose, and a digital
    // voice mode's rate is its decoder's rather than the audio's. Everything
    // else resamples to the audio rate, by a whole factor after the detector.
    //
    // `decimation` above is demod_rate over audio_rate and is not a whole
    // number for a TETRA receiver at 72000 against 48000 of audio. It is not
    // read on this branch, which is why it can be wrong there.
    plan.demod.decimation = complex_output ? 1U : decimation;
    plan.output_rate = complex_output ? plan.demod_rate : plan.audio_rate;

    // The audio band this receiver is delivering, which is not always "as
    // much as the rate carries".
    //
    // Every mode but one wants its passband edge at 0.4 of the audio rate
    // and its stopband at the 0.5 that folds, which is what this always did
    // and what these two lines still compute for them, to the hertz.
    //
    // WFM delivering PROGRAMME audio is the exception: its band stops at
    // 15 kHz by the channel plan and the pilot is at 19 kHz, so the filter
    // has a real stopband edge to hit that has nothing to do with the rate.
    // WFM delivering the MULTIPLEX is not an exception, because the whole
    // point of that receiver is that the composite comes out intact; the
    // predicate is the same audio rate engine::resolve_deemphasis uses, so a
    // composite tap cannot get an audio band and a curve separately.
    const bool programme_audio =
        (plan.mode == kDemodWfm) && plan.audio_rate < engine::kCompositeAudioRateHz;
    const double audio_nyquist = 0.5 * static_cast<double>(plan.audio_rate);
    const double audio_pass_hz =
        programme_audio ? std::min(kFmAudioCeilingHz, 0.8 * audio_nyquist) : 0.8 * audio_nyquist;
    const double audio_stop_hz =
        programme_audio ? std::min(kFmPilotHz, audio_nyquist) : audio_nyquist;

    plan.deemphasis =
        engine::resolve_deemphasis(params.demod, params.deemphasis, plan.audio_rate);
    auto curve = design_deemphasis_taps(plan.deemphasis, plan.audio_rate);
    if (!curve) {
        return std::unexpected(with_context(curve.error(), "plan_vrx de-emphasis"));
    }
    plan.deemphasis_taps = static_cast<std::uint32_t>(curve->taps.size());
    plan.deemphasis_truncation_db = curve->truncation_db;
    plan.deemphasis_truncated_early = curve->truncated_early;

    if (plan.demod.decimation == 1U) {
        plan.audio_decimation_taps = 1U;
        plan.audio_pass_hz = 0.0;
        plan.audio_stop_hz = 0.0;
        plan.audio_stopband_db = 0.0;
    } else {
        // Expressed at the demodulation rate the filter actually runs at.
        const double audio_transition_fraction =
            (audio_stop_hz - audio_pass_hz) / static_cast<double>(plan.demod_rate);
        plan.audio_pass_hz = audio_pass_hz;
        plan.audio_stop_hz = audio_stop_hz;
        std::uint32_t audio_taps =
            std::clamp(kaiser_taps_for(kAudioAttenuationDb, audio_transition_fraction), 3U,
                       kMaxDecimationTaps);
        // Odd, so the group delay is a whole number of samples and the filter
        // is linear phase with no half-sample bookkeeping downstream.
        if ((audio_taps % 2U) == 0U) {
            ++audio_taps;
        }
        plan.audio_decimation_taps = std::min(audio_taps, kMaxDecimationTaps);
        plan.audio_stopband_db =
            -attenuation_reachable(plan.audio_decimation_taps, audio_transition_fraction);
    }

    // The folded length, which is what the kernel loops over and what its
    // ring history has to cover. fold_deemphasis states the arithmetic and
    // refuses a pair that will not fit; asking it here rather than
    // reproducing the expression is what stops the shape and the table
    // disagreeing about a length.
    {
        const std::vector<float> identity(plan.audio_decimation_taps, 0.0F);
        auto folded = fold_deemphasis(ConstRealSpan(identity), ConstRealSpan(curve->taps),
                                      plan.demod.decimation);
        if (!folded) {
            return std::unexpected(with_context(folded.error(), "plan_vrx audio filter"));
        }
        plan.demod.audio_taps = static_cast<std::uint32_t>(folded->size());
    }

    plan.audio_group_delay_demod_samples =
        (static_cast<double>(plan.audio_decimation_taps) - 1.0) / 2.0 +
        static_cast<double>(plan.demod.decimation) * curve->group_delay_audio_samples;

    // Stereo, and the channel count, which is the one number every consumer
    // sizing a buffer reads.
    //
    // The complex taps are two channels for a different reason and have
    // always written two floats per frame; saying so here rather than at four
    // call sites is what stopped stereo being a fifth place to remember.
    plan.stereo = engine::resolve_stereo(params.demod, params.stereo, plan.audio_rate);
    if (complex_output) {
        plan.demod.channels = 2U;
        plan.demod.pilot_taps = 0U;
    } else if (plan.stereo) {
        plan.demod.channels = 2U;
        plan.demod.pilot_taps = stereo_pilot_taps(plan.demod_rate);
        if (plan.demod.pilot_taps == 0) {
            return fail(std::format(
                "plan_vrx: a stereo receiver needs a 19 kHz pilot filter and none can be "
                "designed at {} S/s",
                plan.demod_rate));
        }
    } else {
        plan.demod.channels = 1U;
        plan.demod.pilot_taps = 0U;
    }

    if (plan.mode == kDemodAm) {
        const auto window = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(plan.demod_rate / kAmDcCornerHz, 16, kMaxDcTaps));
        plan.demod.dc_taps = window;
    } else {
        plan.demod.dc_taps = 1U;
    }

    plan.deviation = fm_deviation(plan.mode, plan.bandwidth);
    plan.demod_gain = vrx_demod_gain(plan.mode, plan.demod_rate, plan.deviation);

    return plan;
}

}  // namespace

Expected<VrxPlan> plan_vrx(const GridParams& grid, SampleRate rate,
                           const engine::VrxParams& params,
                           const engine::VrxPlacement& placement) {
    auto designed = design_vrx(grid, rate, params, placement);
    if (!designed) {
        return designed;
    }
    VrxPlan plan = std::move(*designed);

    // The half-width truncates on an odd width, as it always has: the
    // planner passed bandwidth/2 here before edges existed and this is the
    // same quantity. The centre is exact, so an odd width costs half a hertz
    // of width and nothing of position.
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

    const double audio_cutoff_hz =
        (plan.audio_stop_hz > 0.0) ? 0.5 * (plan.audio_pass_hz + plan.audio_stop_hz) : 0.0;
    // The output rate rather than the audio rate, which is the same number
    // for every mode that ends in audio. For a digital voice mode it is the
    // decoder's rate, and passing the engine's audio rate there refused a
    // P25 receiver on any engine configured above 48000 of audio, for being
    // an interpolation, on a table of one unit tap that interpolates nothing.
    auto decimation_table = design_audio_taps(plan.audio_decimation_taps, plan.demod_rate,
                                              plan.output_rate, kAudioAttenuationDb,
                                              audio_cutoff_hz);
    if (!decimation_table) {
        return std::unexpected(with_context(decimation_table.error(), "plan_vrx audio taps"));
    }

    // Designed a second time rather than carried out of design_vrx. It costs
    // a logarithm and at most a thousand multiplies, it is deterministic, and
    // the alternative is a vector on VrxPlan that vrx_shape_for would have to
    // allocate on every retune to answer a question about a tap count.
    auto curve = design_deemphasis_taps(plan.deemphasis, plan.audio_rate);
    if (!curve) {
        return std::unexpected(with_context(curve.error(), "plan_vrx de-emphasis"));
    }
    auto audio_taps_table = fold_deemphasis(ConstRealSpan(*decimation_table),
                                            ConstRealSpan(curve->taps), plan.demod.decimation);
    if (!audio_taps_table) {
        return std::unexpected(with_context(audio_taps_table.error(), "plan_vrx audio taps"));
    }
    if (audio_taps_table->size() != plan.demod.audio_taps) {
        // The shape said one length and the table came out another, which
        // core/engine/vrx_stage.cpp would meet as a bad copy into a buffer
        // sized from the shape rather than as a refusal. Both numbers come
        // from fold_deemphasis, so this fires only if the two calls were
        // handed different inputs.
        return fail(std::format(
            "plan_vrx: the audio filter's shape says {} taps and the table it designed holds "
            "{}",
            plan.demod.audio_taps, audio_taps_table->size()));
    }
    const std::vector<float> dc_weights = design_dc_weights(plan.demod.dc_taps);

    StereoDesign stereo;
    if (plan.demod.pilot_taps != 0) {
        auto tables = design_stereo_tables(plan.demod_rate, plan.demod.audio_taps,
                                           plan.demod.pilot_taps);
        if (!tables) {
            return std::unexpected(with_context(tables.error(), "plan_vrx stereo tables"));
        }
        stereo = std::move(*tables);
        plan.pilot_transition_hz = stereo.pilot_transition_hz;
        plan.pilot_stopband_db = stereo.pilot_stopband_db;
    }

    plan.demod_weights.reserve(audio_taps_table->size() + dc_weights.size() +
                               stereo.pilot.size() + stereo.rotation.size());
    plan.demod_weights.insert(plan.demod_weights.end(), audio_taps_table->begin(),
                              audio_taps_table->end());
    plan.demod_weights.insert(plan.demod_weights.end(), dc_weights.begin(), dc_weights.end());
    plan.demod_weights.insert(plan.demod_weights.end(), stereo.pilot.begin(),
                              stereo.pilot.end());
    plan.demod_weights.insert(plan.demod_weights.end(), stereo.rotation.begin(),
                              stereo.rotation.end());

    return plan;
}

// ---------------------------------------------------------------------------
// The pipeline's shape
// ---------------------------------------------------------------------------

VrxShape shape_of(const VrxPlan& plan) {
    VrxShape shape;
    shape.fine = plan.fine;
    shape.demod = plan.demod;
    shape.channel_rate = plan.channel_rate;
    shape.demod_rate = plan.demod_rate;
    shape.output_rate = plan.output_rate;
    return shape;
}

Expected<VrxShape> vrx_shape_for(const GridParams& grid, SampleRate rate,
                                 const engine::VrxParams& params,
                                 const engine::VrxPlacement& placement) {
    auto designed = design_vrx(grid, rate, params, placement);
    if (!designed) {
        return std::unexpected(designed.error());
    }
    return shape_of(*designed);
}

std::string describe_shape_change(const VrxShape& from, const VrxShape& to) {
    return std::format(
        "this retune changes the receiver's filter shape, not just where it is pointed: "
        "{} taps at {} S/s out at {} S/s becomes {} taps at {} S/s out at {} S/s. Moving the "
        "dial is a push constant and a new tap table, which is free; changing the bandwidth "
        "or the audio rate is a remove and an add",
        from.fine.taps, from.demod_rate, from.output_rate, to.fine.taps, to.demod_rate,
        to.output_rate);
}

std::string describe_audio_chain(const VrxPlan& plan) {
    if (plan.mode == kDemodRaw) {
        return std::format(
            "raw tap: complex baseband at {} S/s, no detector, no audio filter and no "
            "de-emphasis. This is the composite a decoder attaches to, not programme audio",
            plan.output_rate);
    }
    if (is_complex_output(plan.mode)) {
        return std::format(
            "{} tap: complex baseband mixed to DC and filtered to {} to {} Hz, at {} S/s for "
            "the decoder, no detector, no audio filter and no de-emphasis",
            engine::demod_name(static_cast<engine::Demod>(plan.mode)), plan.passband.low,
            plan.passband.high, plan.output_rate);
    }

    std::string text =
        std::format("{} at {} S/s", engine::demod_name(static_cast<engine::Demod>(plan.mode)),
                    plan.output_rate);

    if (plan.audio_decimation_taps > 1) {
        text += std::format(", audio filtered flat to {:.0f} Hz and {:.0f} dB down by "
                            "{:.0f} Hz",
                            plan.audio_pass_hz, -plan.audio_stopband_db, plan.audio_stop_hz);
    } else {
        text += ", no audio filter: the demodulation rate is already the audio rate";
    }

    if (plan.deemphasis == engine::Deemphasis::None) {
        // Naming the reason and not only the state. "No de-emphasis" on a
        // broadcast station is a fault and on a composite tap is correct,
        // and an operator reading one line cannot tell those apart from the
        // word alone.
        if (plan.mode == kDemodWfm) {
            text += std::format(
                ", NO DE-EMPHASIS: this receiver's {} S/s audio is at or above the {} S/s "
                "that carries the whole multiplex, so it is a composite tap and a curve "
                "would pull the 57 kHz data band down 28.6 dB",
                plan.audio_rate, engine::kCompositeAudioRateHz);
        } else {
            text += ", no de-emphasis, which is what this mode's channel plan asks for";
        }
    } else {
        text += std::format(", {} de-emphasis in {} taps folded into the audio filter",
                            engine::deemphasis_name(plan.deemphasis), plan.deemphasis_taps);
        if (plan.deemphasis_truncated_early) {
            text += std::format(
                ". THE CURVE WAS CUT SHORT at {} taps, so it is only {:.0f} dB of the true "
                "one-pole rather than the 144 dB the expansion wanted: the bottom of the "
                "audio band is lifted slightly",
                plan.deemphasis_taps, plan.deemphasis_truncation_db);
        }
    }

    if (plan.stereo) {
        text += std::format(
            ", stereo from a {} tap pilot filter {:.0f} dB down at 15 and 23 kHz. L and R "
            "come back bit-identical on any sample where the 19 kHz pilot is absent, which "
            "is how a mono station reads here rather than as a guess about the sound",
            plan.demod.pilot_taps, -plan.pilot_stopband_db);
    } else if (plan.mode == kDemodWfm && plan.audio_rate < engine::kCompositeAudioRateHz) {
        text += ", MONO: the stereo decoder was turned off on this receiver, so the "
                "difference channel is left in the composite unused";
    }

    if (plan.bandwidth_clamped) {
        text += std::format(
            ". THE PASSBAND WAS CLAMPED to {} to {} Hz by one grid channel, so this is "
            "narrower than what was asked for",
            plan.passband.low, plan.passband.high);
    }

    return text;
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

    // How far back this dispatch's first output reaches.
    //
    // WHAT THIS BLOCK USED TO DO, AND IT WAS A THIRD COPY. It spelled the
    // detector's history out here, added the audio filter's reach to it, and
    // arrived at the same number dsp::validate and core/engine/vrx_stage.cpp
    // each derived separately. FM stereo is what would have separated them:
    // the pilot bandpass is measured at the middle of the audio window and
    // reaches further below it than the window's own edge, by 52 samples on
    // a 48 kHz broadcast receiver, and none of the three expressions knew.
    // The engine compares this against its ring capacity, so the three
    // agreeing is the whole point and they now agree by construction.
    block.oldest_fine = first_fine - static_cast<SampleIndex>(demod_fine_history(plan.demod));

    return block;
}

}  // namespace revenant::dsp
