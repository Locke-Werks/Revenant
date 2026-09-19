// This include must come first. It carries the pragma that disables
// floating-point contraction for this translation unit, and a pragma that
// appears after a function definition does not apply to it. A referee that
// fuses its own multiply-adds gives a different answer when built by a
// different host compiler, and then the diff suite is reporting on the
// harness rather than on the kernel.
#include "core/dsp/reference_fp.h"

#include "core/dsp/spectrum_levels_reference.h"

#include <algorithm>
#include <array>
#include <format>

#include "core/dsp/denormal_mode.h"

namespace revenant::dsp {
namespace {

static_assert(kReferenceFpDisciplineApplied,
              "core/dsp/reference_fp.h must be included before anything else in a reference "
              "implementation");

// The kernel's own bound, written out rather than derived, because the kernel
// writes it out: a twin that computed kBuckets - 1 would not notice a shader
// whose constant had drifted.
constexpr float kTopBucket = 1023.0F;

// floor(count * permille / 1000) without a 64-bit intermediate, because GLSL
// has none here and the twin has to take the same route to reach the same
// integer. count/1000 and count%1000 are both exact.
[[nodiscard]] std::uint32_t rank_of(std::uint32_t count, std::uint32_t permille) {
    return (count / 1000U) * permille + ((count % 1000U) * permille) / 1000U;
}

}  // namespace

std::uint32_t spectrum_levels_bucket_of(float decibels) {
    float t = (decibels - kSpectrumLevelsRangeFloorDb) * kSpectrumLevelsBucketsPerDb;

    // GLSL's bounds here are written as selects rather than as clamp(), and so
    // are these, because clamp and std::clamp do not agree about NaN and
    // writing the comparison makes the agreement visible rather than assumed.
    t = (t < 0.0F) ? 0.0F : t;
    t = (t > kTopBucket) ? kTopBucket : t;

    // Truncation toward zero, which on a non-negative value is the floor.
    // SPIR-V's float-to-unsigned conversion rounds the same way, so this is
    // exact on both sides rather than merely equal in the usual case.
    return static_cast<std::uint32_t>(t);
}

float spectrum_levels_value_of(std::uint32_t bucket) {
    return kSpectrumLevelsRangeFloorDb +
           (static_cast<float>(bucket) + 0.5F) * kSpectrumLevelsDbPerBucket;
}

Status validate(const SpectrumLevelsParams& params) {
    if (params.bins == 0) {
        return fail("spectrum levels needs at least one bin to measure");
    }
    if (params.low_permille > 1000 || params.high_permille > 1000) {
        return fail(std::format("a percentile in parts per thousand cannot exceed 1000, got "
                                "{} and {}",
                                params.low_permille, params.high_permille));
    }
    if (params.low_permille >= params.high_permille) {
        return fail(std::format("the low percentile must sit below the high one, got {} and {} "
                                "per mille",
                                params.low_permille, params.high_permille));
    }
    return {};
}

Status reference_spectrum_levels(const SpectrumLevelsParams& params,
                                 ConstRealSpan frame,
                                 RealSpan levels) {
    if (const auto checked = validate(params); !checked) {
        return std::unexpected(with_context(checked.error(), "reference_spectrum_levels"));
    }
    if (frame.size() < params.bins) {
        return fail(std::format("reference_spectrum_levels was asked for {} bins and given a "
                                "frame of {}",
                                params.bins, frame.size()));
    }
    if (levels.size() < kSpectrumLevelsOutputs) {
        return fail(std::format("reference_spectrum_levels writes {} values and was given room "
                                "for {}",
                                kSpectrumLevelsOutputs, levels.size()));
    }

    // The GPU flushes denormals to zero in fp32 compute and cannot be told not
    // to on every device, so the reference does the same or the two can never
    // agree bit for bit. See core/dsp/denormal_mode.h for why this is the
    // project's policy rather than a workaround for one kernel. Nothing here
    // can produce a denormal from a finite decibel value, so this is the
    // discipline rather than a fix.
    const ScopedDenormalFlush flush_denormals;

    std::array<std::uint32_t, kSpectrumLevelsBuckets> histogram{};
    for (std::uint32_t i = 0; i < params.bins; ++i) {
        ++histogram[spectrum_levels_bucket_of(frame[i])];
    }

    // Zero-based order indices, clamped because a permille of 1000 asks for
    // the element one past the end.
    std::uint32_t low_rank = rank_of(params.bins, params.low_permille);
    std::uint32_t high_rank = rank_of(params.bins, params.high_permille);
    low_rank = std::min(low_rank, params.bins - 1U);
    high_rank = std::min(high_rank, params.bins - 1U);

    std::uint32_t low_bucket = 0;
    std::uint32_t high_bucket = 0;
    bool low_found = false;
    bool high_found = false;

    std::uint32_t cumulative = 0;
    for (std::uint32_t b = 0; b < kSpectrumLevelsBuckets; ++b) {
        cumulative += histogram[b];

        // The element at order index r is in the first bucket whose running
        // total passes r, counting from zero.
        if (!low_found && cumulative > low_rank) {
            low_bucket = b;
            low_found = true;
        }
        if (!high_found && cumulative > high_rank) {
            high_bucket = b;
            high_found = true;
        }
    }

    levels[0] = spectrum_levels_value_of(low_bucket);
    levels[1] = spectrum_levels_value_of(high_bucket);
    return {};
}

}  // namespace revenant::dsp
