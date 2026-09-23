// CPU twins of core/shaders/iq_moments.comp and core/shaders/iq_correct.comp.
//
// Bit-identical to the kernels rather than close to them. Every operation in
// both kernels is a single IEEE add, subtract or multiply under `precise`, so
// each rounds once and the same way on every conformant device; this file
// performs the same operations in the same order with contraction off and
// denormals flushed, which is the whole of what agreement takes. Read each
// function beside its kernel.

#include "core/dsp/reference_fp.h"

#include <format>

#include "core/dsp/denormal_mode.h"
#include "core/dsp/front_end_correction.h"

namespace revenant::dsp {

static_assert(kReferenceFpDisciplineApplied,
              "a reference twin must include core/dsp/reference_fp.h first");

namespace {

[[nodiscard]] Status check_ring(std::size_t ring_size, std::uint32_t ring_mask,
                                std::uint32_t count, const char* what) {
    const std::uint64_t capacity = static_cast<std::uint64_t>(ring_mask) + 1;
    if ((capacity & (capacity - 1)) != 0) {
        return fail(std::format("{}: ring_mask {} is not a power of two less one", what,
                                ring_mask));
    }
    if (ring_size < capacity) {
        return fail(std::format("{}: the ring holds {} samples and the mask names {}", what,
                                ring_size, capacity));
    }
    if (count > capacity) {
        // Both sides would alias the block onto itself identically, so a diff
        // would pass while both were wrong.
        return fail(std::format("{}: a block of {} samples is longer than the {}-sample ring",
                                what, count, capacity));
    }
    return {};
}

}  // namespace

Status reference_iq_moments(ConstComplexSpan ring, const IqMomentsParams& params, RealSpan sums) {
    if (auto ok = check_ring(ring.size(), params.ring_mask, params.count, "reference_iq_moments");
        !ok) {
        return ok;
    }
    const std::uint32_t chunks = iq_moments_chunks(params.count);
    if (sums.size() < static_cast<std::size_t>(chunks) * kIqMomentsPerChunk) {
        return fail(std::format("reference_iq_moments: {} chunks need {} floats and {} were given",
                                chunks, chunks * kIqMomentsPerChunk, sums.size()));
    }

    const ScopedDenormalFlush flush;
    for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
        const std::uint32_t first = chunk * kIqMomentsChunk;
        const std::uint32_t last =
            first + kIqMomentsChunk < params.count ? first + kIqMomentsChunk : params.count;

        float si = 0.0F;
        float sq = 0.0F;
        float sii = 0.0F;
        float sqq = 0.0F;
        float siq = 0.0F;
        for (std::uint32_t k = first; k < last; ++k) {
            // The same unsigned wrap the kernel's 32-bit add takes.
            const Complex32 v = ring[(params.src_offset + k) & params.ring_mask];
            const float i = v.real();
            const float q = v.imag();
            si = si + i;
            sq = sq + q;
            sii = sii + i * i;
            sqq = sqq + q * q;
            siq = siq + i * q;
        }

        const std::size_t at = static_cast<std::size_t>(chunk) * kIqMomentsPerChunk;
        sums[at + 0] = si;
        sums[at + 1] = sq;
        sums[at + 2] = sii;
        sums[at + 3] = sqq;
        sums[at + 4] = siq;
    }
    return {};
}

Status reference_iq_correct(ConstComplexSpan in, const IqCorrectParams& params, ComplexSpan out) {
    if (auto ok = check_ring(in.size(), params.ring_mask, params.count, "reference_iq_correct");
        !ok) {
        return ok;
    }
    if (out.size() < in.size()) {
        return fail(std::format("reference_iq_correct: the output ring holds {} samples and the "
                                "input {}",
                                out.size(), in.size()));
    }

    const ScopedDenormalFlush flush;
    for (std::uint32_t index = 0; index < params.count; ++index) {
        const std::uint32_t slot = (params.offset + index) & params.ring_mask;
        const Complex32 v = in[slot];
        const float i = v.real() - params.dc_i;
        const float q_dc = v.imag() - params.dc_q;
        const float q = params.cross * i + params.scale * q_dc;
        out[slot] = Complex32{i, q};
    }
    return {};
}

}  // namespace revenant::dsp
