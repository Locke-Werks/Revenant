// This include must come first. It carries the pragma that disables
// floating-point contraction for the twins below.
#include "core/dsp/reference_fp.h"

#include "core/dsp/comb_cancel.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>

#include "core/dsp/denormal_mode.h"

namespace revenant::dsp {

static_assert(kReferenceFpDisciplineApplied,
              "core/dsp/reference_fp.h must be included before anything else in a reference "
              "translation unit");

namespace {

[[nodiscard]] bool is_mask(std::uint32_t mask) {
    const std::uint64_t size = static_cast<std::uint64_t>(mask) + 1U;
    return std::has_single_bit(size);
}

[[nodiscard]] std::uint64_t ring_of(std::uint32_t mask) {
    return static_cast<std::uint64_t>(mask) + 1U;
}

constexpr std::uint32_t kTableShift = 32U - kCombTableLog2;

// comb_project.comp's kChunk.
constexpr std::uint32_t kProjectChunk = 64;

[[nodiscard]] Complex32 tone(ConstComplexSpan table, const CombLine& line, std::uint32_t n) {
    // uint32 arithmetic wraps modulo 2^32, the kernel's phase.
    const std::uint32_t phase = line.phase0 + n * line.step;
    return table[phase >> kTableShift];
}

}  // namespace

std::uint32_t comb_step(double frequency_hz, SampleRate rate) {
    if (rate <= 0 || !std::isfinite(frequency_hz)) {
        return 0;
    }
    constexpr double kCycle = 4294967296.0;
    double steps = std::round(frequency_hz / static_cast<double>(rate) * kCycle);
    steps = std::fmod(steps, kCycle);
    if (steps < 0.0) {
        steps += kCycle;
    }
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(steps));
}

std::vector<Complex32> comb_table() {
    std::vector<Complex32> out(kCombTableSize);
    for (std::uint32_t i = 0; i < kCombTableSize; ++i) {
        const double angle =
            2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(kCombTableSize);
        out[i] = Complex32{static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
    return out;
}

Status validate(const CombParams& params) {
    if (!is_mask(params.ring_mask) || !is_mask(params.removed_mask)) {
        return fail("comb cancel: the ring and removed masks must each be a power of two minus one");
    }
    // The removed ring is indexed from the sample ring's slot under its own
    // mask, which only names the same sample when it divides the sample ring.
    if (ring_of(params.removed_mask) > ring_of(params.ring_mask)) {
        return fail("comb cancel: the removed ring must be no larger than the sample ring");
    }
    if (params.lines > kCombMaxLines) {
        return fail(std::format("comb cancel: {} lines is more than the {} the stage cancels",
                                params.lines, kCombMaxLines));
    }
    if (params.sub_blocks < 1 || params.sub_blocks > kCombMaxSubBlocks) {
        return fail(std::format("comb cancel: {} sub-blocks is outside [1, {}]",
                                params.sub_blocks, kCombMaxSubBlocks));
    }
    if (params.count == 0 || params.count % params.sub_blocks != 0) {
        return fail(std::format("comb cancel: {} samples do not split into {} equal non-empty "
                                "sub-blocks",
                                params.count, params.sub_blocks));
    }
    if (params.count > ring_of(params.removed_mask)) {
        return fail(std::format("comb cancel: {} samples do not fit the removed ring of {}",
                                params.count, ring_of(params.removed_mask)));
    }
    return {};
}

Status reference_comb_project(const CombParams& params, ConstComplexSpan raw,
                              std::span<const CombLine> lines, ConstComplexSpan table,
                              ComplexSpan projections) {
    if (auto valid = validate(params); !valid) {
        return valid;
    }
    if (raw.size() < ring_of(params.ring_mask) || lines.size() < params.lines ||
        table.size() != kCombTableSize ||
        projections.size() < static_cast<std::size_t>(params.lines) * params.sub_blocks) {
        return fail("reference_comb_project: a buffer is shorter than its parameters");
    }
    const ScopedDenormalFlush flush_denormals;
    const std::uint32_t length = params.count / params.sub_blocks;

    for (std::uint32_t l = 0; l < params.lines; ++l) {
        for (std::uint32_t s = 0; s < params.sub_blocks; ++s) {
            const std::uint32_t begin = s * length;
            // The kernel's two levels: chunks of kProjectChunk from zero in
            // sample order, their sums added from zero in chunk order.
            float total_re = 0.0F;
            float total_im = 0.0F;
            for (std::uint32_t c = 0; c < length; c += kProjectChunk) {
                const std::uint32_t end = std::min(c + kProjectChunk, length);
                float chunk_re = 0.0F;
                float chunk_im = 0.0F;
                for (std::uint32_t m = c; m < end; ++m) {
                    const std::uint32_t n = begin + m;
                    const Complex32 x = raw[(params.first + n) & params.ring_mask];
                    const Complex32 t = tone(table, lines[l], n);
                    const float re = x.real() * t.real() + x.imag() * t.imag();
                    const float im = x.imag() * t.real() - x.real() * t.imag();
                    chunk_re = chunk_re + re;
                    chunk_im = chunk_im + im;
                }
                total_re = total_re + chunk_re;
                total_im = total_im + chunk_im;
            }
            projections[static_cast<std::size_t>(l) * params.sub_blocks + s] =
                Complex32{total_re, total_im};
        }
    }
    return {};
}

Status reference_comb_subtract(const CombParams& params, ConstComplexSpan raw,
                               std::span<const CombLine> lines, ConstComplexSpan table,
                               ComplexSpan cleaned, ComplexSpan removed) {
    if (auto valid = validate(params); !valid) {
        return valid;
    }
    if (raw.size() < ring_of(params.ring_mask) || cleaned.size() < ring_of(params.ring_mask) ||
        lines.size() < params.lines || table.size() != kCombTableSize ||
        removed.size() < ring_of(params.removed_mask)) {
        return fail("reference_comb_subtract: a buffer is shorter than its parameters");
    }
    const ScopedDenormalFlush flush_denormals;

    for (std::uint32_t n = 0; n < params.count; ++n) {
        const std::uint32_t slot = (params.first + n) & params.ring_mask;
        const Complex32 x = raw[slot];

        // From negative zero, the value that adds to anything without
        // changing it, so no lines removes -0. See the kernel's header.
        float sum_re = -0.0F;
        float sum_im = -0.0F;
        for (std::uint32_t l = 0; l < params.lines; ++l) {
            const Complex32 t = tone(table, lines[l], n);
            const Complex32 a = lines[l].amplitude;
            const float re = a.real() * t.real() - a.imag() * t.imag();
            const float im = a.real() * t.imag() + a.imag() * t.real();
            sum_re = sum_re + re;
            sum_im = sum_im + im;
        }

        if (params.lines == 0) {
            cleaned[slot] = x;
        } else {
            cleaned[slot] = Complex32{x.real() - sum_re, x.imag() - sum_im};
        }
        removed[(params.first + n) & params.removed_mask] = Complex32{sum_re, sum_im};
    }
    return {};
}

}  // namespace revenant::dsp
