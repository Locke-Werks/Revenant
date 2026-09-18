// This include must come first. It carries the pragma that disables
// floating-point contraction for this translation unit, and a pragma that
// appears after a function definition does not apply to it. A referee that
// fuses its own multiply-adds gives a different answer when built by a
// different host compiler, and then the diff suite is reporting on the harness
// rather than on the kernel.
#include "core/dsp/reference_fp.h"

#include "core/dsp/pfb_fft_reference.h"

#include <format>
#include <vector>

#include "core/dsp/denormal_mode.h"

namespace revenant::dsp {
namespace {

static_assert(kReferenceFpDisciplineApplied,
              "core/dsp/reference_fp.h must be included before anything else in a reference "
              "implementation");

[[nodiscard]] bool is_power_of_two(std::uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

// Transcribes the shader's reversed_index(). Reverses the low `stages` bits,
// which is the load permutation that makes a decimation-in-time transform come
// out in natural order. Pure integer work: a permutation moves no bits of a
// float, so nothing here can perturb the bit-exact comparison.
//
// Written as a loop rather than as the shader's bitfieldReverse followed by a
// shift, because C++ has no bit-reversal intrinsic in <bit> and the loop is
// the form a reviewer can check against the GLSL by inspection.
[[nodiscard]] std::uint32_t reversed_index(std::uint32_t index, std::uint32_t stages) {
    std::uint32_t reversed = 0;
    for (std::uint32_t bit = 0; bit < stages; ++bit) {
        reversed |= ((index >> bit) & 1u) << (stages - 1u - bit);
    }
    return reversed;
}

// One transform, transcribing the shader's shared-memory load and stage loop.
// `scratch` is the shader's `shared vec2 scratch[kChannels]`; the caller gives
// it whatever storage it wants the result in, which is how both public entry
// points share one butterfly graph.
//
// The shader runs the butterflies of a stage strided across its workgroup and
// this runs them ascending. That is not a divergence: within a stage the
// (ia, ib) pairs are disjoint and each butterfly reads only its own two slots,
// so the values produced are a function of (M, stage, butterfly index) alone
// and not of the order the butterflies were visited. That invariant is what
// the workgroup-size sweep in the test suite asserts on the device side.
void transform_block(ConstComplexSpan input,
                     ConstComplexSpan twiddles,
                     ComplexSpan scratch,
                     std::uint32_t channels,
                     std::uint32_t stages) {
    for (std::uint32_t i = 0; i < channels; ++i) {
        scratch[i] = input[reversed_index(i, stages)];
    }

    const std::uint32_t butterflies = channels >> 1u;

    for (std::uint32_t stage = 0; stage < stages; ++stage) {
        const std::uint32_t half_span = 1u << stage;
        const std::uint32_t twiddle_step = channels >> (stage + 1u);

        for (std::uint32_t b = 0; b < butterflies; ++b) {
            const std::uint32_t j = b & (half_span - 1u);
            const std::uint32_t ia = ((b >> stage) << (stage + 1u)) + j;
            const std::uint32_t ib = ia + half_span;

            const float a_real = scratch[ia].real();
            const float a_imag = scratch[ia].imag();
            const float b_real = scratch[ib].real();
            const float b_imag = scratch[ib].imag();
            const float w_real = twiddles[j * twiddle_step].real();
            const float w_imag = twiddles[j * twiddle_step].imag();

            // Written out rather than using std::complex's arithmetic, for the
            // same two reasons complex_ops.cpp gives. The order has to match
            // the shader exactly: multiply, multiply, subtract; multiply,
            // multiply, add; then the four butterfly adds. std::complex is
            // free to evaluate a product however the implementation likes, and
            // one that handles the Annex G infinity and NaN cases does extra
            // work with extra rounding that the shader does not do. And being
            // explicit is what makes the comparison auditable: these six lines
            // sit beside the shader's six and a reader can see they are the
            // same computation.
            const float value_real = b_real * w_real - b_imag * w_imag;
            const float value_imag = b_real * w_imag + b_imag * w_real;
            const float sum_real = a_real + value_real;
            const float sum_imag = a_imag + value_imag;
            const float diff_real = a_real - value_real;
            const float diff_imag = a_imag - value_imag;

            scratch[ia] = Complex32{sum_real, sum_imag};
            scratch[ib] = Complex32{diff_real, diff_imag};
        }
    }
}

}  // namespace

Status validate(const PfbFftParams& params) {
    if (!is_power_of_two(params.channels) || params.channels < 2) {
        return fail(std::format(
            "pfb fft channels must be a power of two of at least 2, got {}", params.channels));
    }
    if (params.stages >= 32 || (1u << params.stages) != params.channels) {
        return fail(std::format("pfb fft stages must be log2(channels): {} channels needs {}, "
                                "got {}",
                                params.channels, fft_stages(params.channels), params.stages));
    }
    if (params.decimation == 0 || params.decimation > params.channels ||
        params.channels % params.decimation != 0) {
        return fail(std::format("pfb fft decimation must divide channels: {} does not divide {}",
                                params.decimation, params.channels));
    }
    if (!is_power_of_two(params.out_ring_blocks)) {
        return fail(std::format("pfb fft out_ring_blocks must be a non-zero power of two, got {}",
                                params.out_ring_blocks));
    }
    if (params.out_ring_mask != params.out_ring_blocks - 1u) {
        return fail(std::format("pfb fft out_ring_mask must be out_ring_blocks - 1: {} blocks "
                                "needs mask {}, got {}",
                                params.out_ring_blocks, params.out_ring_blocks - 1u,
                                params.out_ring_mask));
    }
    // Two workgroups writing one ring slot have no ordering between them, so
    // the device answer would depend on scheduling and nothing could be
    // bit-exact against it. The dispatch is too large for the ring, which is a
    // sizing mistake rather than a race to be fixed with a barrier.
    if (params.block_count > params.out_ring_blocks) {
        return fail(std::format("pfb fft dispatch of {} blocks overruns a {}-block ring: blocks "
                                "would alias and the device write order is unspecified",
                                params.block_count, params.out_ring_blocks));
    }
    return {};
}

Status reference_pfb_fft(const PfbFftParams& params,
                         ConstComplexSpan branch_values,
                         ConstComplexSpan twiddles,
                         ComplexSpan channel_ring) {
    if (const auto checked = validate(params); !checked) {
        return std::unexpected(with_context(checked.error(), "reference_pfb_fft"));
    }

    const auto channels = static_cast<std::size_t>(params.channels);
    const auto blocks = static_cast<std::size_t>(params.block_count);

    if (twiddles.size() != channels) {
        return fail(std::format("reference_pfb_fft needs exactly {} twiddles for {} channels, "
                                "got {}",
                                channels, channels, twiddles.size()));
    }
    if (branch_values.size() < blocks * channels) {
        return fail(std::format("reference_pfb_fft needs {} branch values for {} blocks of {} "
                                "channels, got {}",
                                blocks * channels, blocks, channels, branch_values.size()));
    }
    const std::size_t ring_size = channels * static_cast<std::size_t>(params.out_ring_blocks);
    if (channel_ring.size() < ring_size) {
        return fail(std::format("reference_pfb_fft needs a channel ring of {} ({} channels by {} "
                                "blocks), got {}",
                                ring_size, channels, params.out_ring_blocks,
                                channel_ring.size()));
    }

    // The GPU flushes denormals to zero in fp32 compute and cannot be told not
    // to on every device, so the reference does the same or the two can never
    // agree bit for bit. See core/dsp/denormal_mode.h for why this is the
    // project's policy rather than a workaround for one kernel.
    //
    // Nothing in the butterfly chain can feed a denormal in from inside: every
    // producer above flushed its own result to exact zero, and the twiddle
    // table holds no denormals because |W| = 1. The flush is here for the
    // inputs, which come from outside.
    const ScopedDenormalFlush flush_denormals;

    std::vector<Complex32> scratch(channels, Complex32{});

    for (std::uint32_t block = 0; block < params.block_count; ++block) {
        const std::size_t block_origin = static_cast<std::size_t>(block) * channels;
        transform_block(branch_values.subspan(block_origin, channels), twiddles, scratch,
                        params.channels, params.stages);

        // The addition wraps modulo 2^32 exactly as the kernel's does, and for
        // the same reason it is harmless: both uses below reduce it modulo a
        // power of two that divides 2^32.
        const std::uint32_t m_abs = params.block_base + block;
        const std::uint32_t slot = m_abs & params.out_ring_mask;

        // (m*D) mod M on the already-reduced m, so the product cannot overflow
        // 32 bits. M is a power of two, so both reductions are exact.
        const std::uint32_t dm =
            ((m_abs & (params.channels - 1u)) * params.decimation) & (params.channels - 1u);

        for (std::uint32_t k = 0; k < params.channels; ++k) {
            Complex32 value = scratch[k];

            // exp(-j*2*pi*k*m*D/M). Identically 1 at D == M, which is why the
            // shader's specialization constant folds the whole branch away
            // there. At D == M/2 it is (-1)^(k*m), a sign flip, exact in
            // floating point. The multiply is still written out in full rather
            // than special-cased into a negation: the kernel does the multiply,
            // and -0.0 is a bit pattern the diff can see.
            if (params.decimation != params.channels) {
                const Complex32 correction = twiddles[(k * dm) & (params.channels - 1u)];
                const float correction_real = correction.real();
                const float correction_imag = correction.imag();
                const float value_real = value.real();
                const float value_imag = value.imag();

                const float product_real =
                    value_real * correction_real - value_imag * correction_imag;
                const float product_imag =
                    value_real * correction_imag + value_imag * correction_real;
                value = Complex32{product_real, product_imag};
            }

            // slot is already reduced, so the mask inside the helper is a
            // no-op here. It is called anyway so that the ring layout is
            // written down in exactly one place.
            channel_ring[channel_ring_index(params, k, slot)] = value;
        }
    }

    return {};
}

Status reference_fft_radix2(ConstComplexSpan twiddles,
                            ConstComplexSpan input,
                            ComplexSpan output,
                            std::uint32_t transform_size,
                            std::uint32_t batch_count) {
    if (!is_power_of_two(transform_size) || transform_size < 2) {
        return fail(std::format(
            "reference_fft_radix2 transform size must be a power of two of at least 2, got {}",
            transform_size));
    }

    const auto size = static_cast<std::size_t>(transform_size);
    const std::size_t total = size * static_cast<std::size_t>(batch_count);

    if (twiddles.size() != size) {
        return fail(std::format("reference_fft_radix2 needs exactly {} twiddles, got {}", size,
                                twiddles.size()));
    }
    if (input.size() < total || output.size() < total) {
        return fail(std::format("reference_fft_radix2 needs {} values in and out for {} "
                                "transforms of {}, got {} in and {} out",
                                total, batch_count, size, input.size(), output.size()));
    }

    const ScopedDenormalFlush flush_denormals;

    const std::uint32_t stages = fft_stages(transform_size);

    for (std::uint32_t block = 0; block < batch_count; ++block) {
        const std::size_t block_origin = static_cast<std::size_t>(block) * size;
        // The output subspan plays the part of the shader's shared array. The
        // spans must not alias, which the header states, because the load
        // permutation would otherwise read values it had already overwritten.
        transform_block(input.subspan(block_origin, size), twiddles,
                        output.subspan(block_origin, size), transform_size, stages);
    }

    return {};
}

}  // namespace revenant::dsp
