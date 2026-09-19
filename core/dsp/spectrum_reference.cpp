// This include must come first. It carries the pragma that disables
// floating-point contraction for this translation unit, and a pragma that
// appears after a function definition does not apply to it. A referee that
// fuses its own multiply-adds gives a different answer when built by a
// different host compiler, and then the diff suite is reporting on the
// harness rather than on the kernel.
#include "core/dsp/reference_fp.h"

#include "core/dsp/spectrum_reference.h"

#include <bit>
#include <cmath>
#include <format>
#include <numbers>
#include <vector>

#include "core/dsp/denormal_mode.h"
#include "core/dsp/vrx_reference.h"

namespace revenant::dsp {
namespace {

static_assert(kReferenceFpDisciplineApplied,
              "core/dsp/reference_fp.h must be included before anything else in a reference "
              "implementation");

[[nodiscard]] bool is_power_of_two(std::uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

// Every constant below is written with the same decimal digits as its
// counterpart in core/shaders/spectrum.comp, so both sides round to the same
// float. Duplicated rather than shared because GLSL has no way to read a C++
// header, which is the same trade core/dsp/vrx_reference.cpp makes for the
// atan coefficients.

// 10*log10(2). Power in decibels is 10*log10(p) and the logarithm is base
// two, so the conversion is one multiply.
constexpr float kDecibelsPerOctave = 3.01029995663981195F;

// (2/ln 2) / (2i+1), the atanh series for a base-two logarithm.
constexpr float kLogC0 = 2.88539008177792681F;
constexpr float kLogC1 = 0.96179669392597560F;
constexpr float kLogC2 = 0.57707801635558536F;
constexpr float kLogC3 = 0.41219858311113240F;
constexpr float kLogC4 = 0.32059889797532520F;

constexpr float kSqrt2F = 1.41421356237309505F;

// Four-term Blackman-Harris, the coefficients as published.
constexpr double kBlackmanHarrisA0 = 0.35875;
constexpr double kBlackmanHarrisA1 = 0.48829;
constexpr double kBlackmanHarrisA2 = 0.14128;
constexpr double kBlackmanHarrisA3 = 0.01168;

}  // namespace

float det_log2(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);

    // Both of these are bit selections, so they are exact and cannot perturb
    // the comparison. The mantissa comes back in [1, 2) with the original
    // exponent replaced by zero.
    int exponent = static_cast<int>((bits >> 23U) & 0xFFU) - 127;
    float mantissa = std::bit_cast<float>((bits & 0x007FFFFFU) | 0x3F800000U);

    // Fold into [1/sqrt(2), sqrt(2)], which holds |t| below 0.1715728753 and
    // is what makes five series terms enough. The halving is an exponent
    // decrement and the increment compensates it, so no mantissa bit moves.
    if (mantissa > kSqrt2F) {
        mantissa = mantissa * 0.5F;
        exponent = exponent + 1;
    }

    const float t = (mantissa - 1.0F) * det_recip(mantissa + 1.0F);
    const float u = t * t;

    float p = kLogC4;
    p = p * u + kLogC3;
    p = p * u + kLogC2;
    p = p * u + kLogC1;
    p = p * u + kLogC0;

    const float fraction = t * p;
    return static_cast<float>(exponent) + fraction;
}

Status validate(const SpectrumParams& params) {
    if (!is_power_of_two(params.channels) || params.channels < 2) {
        return fail(std::format(
            "spectrum channels must be a power of two of at least 2, got {}", params.channels));
    }
    if (!is_power_of_two(params.transform) || params.transform < 4) {
        return fail(std::format("spectrum transform must be a power of two of at least 4, got {}",
                                params.transform));
    }
    if (params.transform > kMaxSpectrumTransform) {
        return fail(std::format(
            "a {}-point spectrum transform needs a twiddle circle of the same length and "
            "build_twiddles serves at most {}",
            params.transform, kMaxSpectrumTransform));
    }
    if (params.stages >= 32 || (1u << params.stages) != params.transform) {
        return fail(std::format("spectrum stages must be log2(transform): {} points needs {}, "
                                "got {}",
                                params.transform, fft_stages(params.transform), params.stages));
    }
    if (!is_power_of_two(params.chan_blocks)) {
        return fail(std::format("spectrum chan_blocks must be a non-zero power of two, got {}",
                                params.chan_blocks));
    }
    if (params.chan_mask != params.chan_blocks - 1u) {
        return fail(std::format("spectrum chan_mask must be chan_blocks - 1: {} blocks needs "
                                "mask {}, got {}",
                                params.chan_blocks, params.chan_blocks - 1u, params.chan_mask));
    }
    // A window longer than the ring reads the same slots twice, so the
    // transform would see a sequence the stream never contained and the frame
    // would show sidebands that are not there. It is a sizing mistake in the
    // caller, not something the kernel can recover from.
    if (params.transform > params.chan_blocks) {
        return fail(std::format(
            "a {}-point window does not fit a channel ring of {} blocks: the transform would "
            "read the same samples more than once",
            params.transform, params.chan_blocks));
    }
    return {};
}

Expected<std::vector<float>> build_spectrum_window(std::uint32_t size) {
    if (size < 4 || !is_power_of_two(size)) {
        return fail(std::format(
            "build_spectrum_window: {} is not a power of two of at least 4", size));
    }

    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    const auto count = static_cast<double>(size);

    std::vector<double> taps(size, 0.0);
    double sum = 0.0;
    for (std::uint32_t n = 0; n < size; ++n) {
        // Periodic rather than symmetric: the divisor is N and not N-1,
        // because this transforms a window of a continuing stream and not an
        // isolated record. The symmetric form leaves a discontinuity of one
        // sample's worth between consecutive frames.
        const double phase = kTwoPi * static_cast<double>(n) / count;
        const double value = kBlackmanHarrisA0 - kBlackmanHarrisA1 * std::cos(phase) +
                             kBlackmanHarrisA2 * std::cos(2.0 * phase) -
                             kBlackmanHarrisA3 * std::cos(3.0 * phase);
        taps[n] = value;
        sum += value;
    }

    if (!(sum > 0.0)) {
        return fail(std::format("build_spectrum_window: the {}-tap window sums to {}, which "
                                "cannot be normalised",
                                size, sum));
    }

    // Coherent gain of one. A complex tone of amplitude A sitting exactly on a
    // bin centre then transforms to magnitude A, so full scale reads 0 dB and
    // the decibel figures in a frame mean dBFS rather than dB relative to
    // whichever window happened to be chosen.
    std::vector<float> normalised(size, 0.0F);
    for (std::uint32_t n = 0; n < size; ++n) {
        normalised[n] = static_cast<float>(taps[n] / sum);
    }
    return normalised;
}

Status reference_spectrum(const SpectrumParams& params,
                          ConstComplexSpan channel_ring,
                          ConstComplexSpan twiddles,
                          ConstRealSpan window,
                          RealSpan frame) {
    if (const auto checked = validate(params); !checked) {
        return std::unexpected(with_context(checked.error(), "reference_spectrum"));
    }

    const auto transform = static_cast<std::size_t>(params.transform);
    const auto channels = static_cast<std::size_t>(params.channels);

    if (twiddles.size() != transform) {
        return fail(std::format("reference_spectrum needs exactly {} twiddles for a {}-point "
                                "transform, got {}",
                                transform, transform, twiddles.size()));
    }
    if (window.size() != transform) {
        return fail(std::format("reference_spectrum needs exactly {} window taps, got {}",
                                transform, window.size()));
    }
    const std::size_t ring_size = channels * static_cast<std::size_t>(params.chan_blocks);
    if (channel_ring.size() < ring_size) {
        return fail(std::format("reference_spectrum needs a channel ring of {} ({} channels by "
                                "{} blocks), got {}",
                                ring_size, channels, params.chan_blocks, channel_ring.size()));
    }
    const auto bins = static_cast<std::size_t>(
        spectrum_bin_count(params.channels, params.transform));
    if (frame.size() < bins) {
        return fail(std::format("reference_spectrum needs a frame of {} bins ({} channels by {} "
                                "kept bins), got {}",
                                bins, channels, spectrum_bins_per_channel(params.transform),
                                frame.size()));
    }

    // The GPU flushes denormals to zero in fp32 compute and cannot be told not
    // to on every device, so the reference does the same or the two can never
    // agree bit for bit. See core/dsp/denormal_mode.h for why this is the
    // project's policy rather than a workaround for one kernel.
    const ScopedDenormalFlush flush_denormals;

    // Hoisted out of the channel loop: this runs over every channel of every
    // frame and the allocation is not part of what is being refereed.
    std::vector<Complex32> windowed(transform, Complex32{});
    std::vector<Complex32> transformed(transform, Complex32{});

    const std::uint32_t half_bins = spectrum_bins_per_channel(params.transform);

    for (std::uint32_t channel = 0; channel < params.channels; ++channel) {
        const std::size_t origin = static_cast<std::size_t>(channel) * params.chan_blocks;

        // The kernel windows the natural-order sample and then permutes it
        // into shared memory. Applying the window here and handing the result
        // to the already-refereed transform is the same two multiplies on the
        // same value in the same order, and it means this file does not carry
        // a second copy of the butterfly graph.
        for (std::uint32_t n = 0; n < params.transform; ++n) {
            const std::size_t slot = (params.in_offset + n) & params.chan_mask;
            const Complex32 sample = channel_ring[origin + slot];
            const float gain = window[n];

            const float real = sample.real() * gain;
            const float imag = sample.imag() * gain;
            windowed[n] = Complex32{real, imag};
        }

        if (auto ran = reference_fft_radix2(twiddles, windowed, transformed, params.transform, 1);
            !ran) {
            return std::unexpected(
                with_context(ran.error(), std::format("reference_spectrum channel {}", channel)));
        }

        const std::size_t out_origin =
            static_cast<std::size_t>(spectrum_channel_slot(params.channels, channel)) * half_bins;

        for (std::uint32_t bin = 0; bin < half_bins; ++bin) {
            const Complex32 value = transformed[spectrum_source_bin(params.transform, bin)];

            const float power = value.real() * value.real() + value.imag() * value.imag();

            // GLSL's max(x, y) is specified as "y if x < y, otherwise x",
            // written out here rather than called as std::fmax, which carries
            // NaN rules GLSL does not. On the finite values a transform of
            // real samples produces the two agree, and writing the comparison
            // makes that visible rather than assumed.
            const float floored = (power < kSpectrumPowerFloor) ? kSpectrumPowerFloor : power;

            frame[out_origin + bin] = det_log2(floored) * kDecibelsPerOctave;
        }
    }

    return {};
}

}  // namespace revenant::dsp
