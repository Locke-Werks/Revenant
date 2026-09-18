// This include must come first. It carries the pragma that disables
// floating-point contraction for this translation unit, and a pragma that
// appears after a function definition does not apply to it.
#include "core/dsp/reference_fp.h"

#include "core/dsp/pfb_branch_reference.h"

#include <cstddef>
#include <format>

#include "core/dsp/denormal_mode.h"

namespace revenant::dsp {
namespace {

static_assert(kReferenceFpDisciplineApplied,
              "core/dsp/reference_fp.h must be included before anything else in a reference "
              "implementation");

}  // namespace

Status reference_pfb_branches(const GridParams& grid,
                              ConstRealSpan prototype_taps,
                              ConstComplexSpan iq_ring,
                              const PfbBranchParams& params,
                              ComplexSpan branch_output) {
    // The grid's own shape is the contract's business: a power-of-two channel
    // count, a decimation that divides it, a nonzero tap count. Calling
    // validate here rather than restating those rules means the reference
    // rejects exactly what the kernels reject, and keeps rejecting it if the
    // rules move.
    if (const auto valid = validate(grid); !valid) {
        return std::unexpected(with_context(valid.error(), "reference_pfb_branches grid"));
    }

    const std::uint32_t channels = grid.channels;
    const std::uint32_t taps = grid.taps_per_branch;
    const std::uint32_t decimation = grid.decimation;

    if (prototype_taps.size() != grid.prototype_length()) {
        return fail(std::format("reference_pfb_branches wants {} prototype taps, got {}",
                                grid.prototype_length(), prototype_taps.size()));
    }

    const std::size_t expected_output =
        static_cast<std::size_t>(params.block_count) * static_cast<std::size_t>(channels);
    if (branch_output.size() != expected_output) {
        return fail(std::format("reference_pfb_branches wants {} branch values, got {}",
                                expected_output, branch_output.size()));
    }

    if (params.block_count == 0) {
        return {};
    }

    // The mask is the ring capacity minus one, and the capacity has to be a
    // power of two or the wrap the kernel relies on is not a modulo at all. A
    // mask that is not all-ones-below-a-bit is the one input that makes this
    // whole file quietly wrong rather than loudly wrong.
    const std::uint64_t capacity = static_cast<std::uint64_t>(params.ring_mask) + 1U;
    if ((capacity & static_cast<std::uint64_t>(params.ring_mask)) != 0U) {
        return fail(std::format("reference_pfb_branches ring_mask {} is not a power of two "
                                "minus one",
                                params.ring_mask));
    }

    if (iq_ring.size() < capacity) {
        return fail(std::format("reference_pfb_branches ring holds {} samples, mask {} implies "
                                "a capacity of {}",
                                iq_ring.size(), params.ring_mask, capacity));
    }

    // This dispatch reads the window x[base - N + 1 .. base + (blocks-1)*D],
    // which is N + (blocks-1)*D distinct samples: the FIR support plus the
    // advance. A ring smaller than that aliases two different samples onto one
    // slot, and the kernel would do the same thing bit for bit, so a diff test
    // would pass while both sides read the wrong history. It is checked here
    // because the reference is the only side that can see the whole request.
    const std::uint64_t span_needed = static_cast<std::uint64_t>(grid.prototype_length()) +
                                      static_cast<std::uint64_t>(params.block_count - 1U) *
                                          static_cast<std::uint64_t>(decimation);
    if (span_needed > capacity) {
        return fail(std::format("reference_pfb_branches needs {} samples of history for {} "
                                "blocks, ring capacity is {}",
                                span_needed, params.block_count, capacity));
    }

    // The device flushes denormals to zero in fp32 and, on the hardware this
    // project runs on, cannot be told not to. The reference models the hardware
    // rather than the other way round. See core/dsp/denormal_mode.h.
    const ScopedDenormalFlush flush_denormals;

    // The kernel runs one invocation per (block, branch), decomposing a flat id
    // as block = id / M and r = id % M. This enumerates the same pairs in the
    // same order. Nothing about the bits depends on that order, since each
    // output is written once by one invocation, but a reader can put the two
    // loops side by side.
    for (std::uint32_t block = 0; block < params.block_count; ++block) {
        // Deliberately 32-bit and deliberately allowed to wrap. The kernel has
        // no 64-bit integer, so this is the arithmetic the device performs, and
        // the twin performs it too rather than computing the mathematically
        // intended index some wider way.
        const std::uint32_t base = params.base_offset + block * decimation;

        for (std::uint32_t r = 0; r < channels; ++r) {
            float acc_real = 0.0F;
            float acc_imag = 0.0F;

            // Ascending q, one accumulate-multiply-add per tap, matching the
            // kernel's loop exactly. Two legitimate orderings of this same sum
            // differ by up to 110592 ULP, so the order is the claim.
            for (std::uint32_t q = 0; q < taps; ++q) {
                const std::uint32_t n = q * channels + r;

                // (base - n) & ring_mask, unsigned and wrapping, exactly as the
                // kernel writes it. When n exceeds base the subtraction wraps
                // modulo 2^32; the capacity is a power of two dividing 2^32, so
                // masking lands on the sample the unwrapped arithmetic names.
                const std::size_t index =
                    static_cast<std::size_t>((base - n) & params.ring_mask);

                const Complex32 s = iq_ring[index];
                const float c = prototype_taps[n];

                acc_real = acc_real + s.real() * c;
                acc_imag = acc_imag + s.imag() * c;
            }

            // The branch reversal, which is the off-by-one. Branch r's result
            // goes to FFT input slot (M - r) mod M so that the next stage can
            // be an ordinary forward transform; the derivation is in
            // core/shaders/pfb_branch.comp. Get it wrong and every channel
            // comes out as channel M-k, the spectrum is mirrored, and nothing
            // downstream looks obviously broken.
            const std::uint32_t slot = (channels - r) & (channels - 1U);

            const std::size_t out_index =
                static_cast<std::size_t>(block) * static_cast<std::size_t>(channels) +
                static_cast<std::size_t>(slot);
            branch_output[out_index] = Complex32{acc_real, acc_imag};
        }
    }

    return {};
}

}  // namespace revenant::dsp
