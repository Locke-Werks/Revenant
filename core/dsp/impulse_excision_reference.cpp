// This include must come first. It carries the pragma that disables
// floating-point contraction for the twins below.
#include "core/dsp/reference_fp.h"

#include "core/dsp/impulse_excision.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>

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

// The kernel's negative zero, the value that adds to anything without
// changing it. See "Raw plus removed" in the header.
constexpr Complex32 kNothing{-0.0F, -0.0F};

// Whether a flag at `rel` samples past `first` counts: it must not be from
// before the restart, where the flag ring holds a previous run's values.
[[nodiscard]] bool flag_at(const ExcisionParams& params, std::span<const std::uint32_t> flags,
                           std::int64_t rel) {
    if (rel + params.history < 0) {
        return false;
    }
    // The kernel's uint(rel) wraps a negative offset modulo 2^32, which the
    // mask then brings back into the ring because the ring divides 2^32.
    const auto offset = static_cast<std::uint32_t>(rel);
    return flags[(params.first + offset) & params.flag_mask] != 0U;
}

// Whether a flag at f in [rel - lead, rel + hang] marks this sample, which is
// a flag in [rel - hang, rel + lead] seen from the sample.
[[nodiscard]] bool marked(const ExcisionConfig& config, const ExcisionParams& params,
                          std::span<const std::uint32_t> flags, std::int64_t rel) {
    for (std::uint32_t m = 0; m <= config.hang + config.lead; ++m) {
        if (flag_at(params, flags, rel - static_cast<std::int64_t>(config.hang) + m)) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------------

ExcisionConfig design_excision(SampleRate source_rate, double max_width_seconds) {
    ExcisionConfig config;
    const double rate = static_cast<double>(std::max<SampleRate>(source_rate, 1));

    const double width = std::round(max_width_seconds * rate);
    config.max_width = static_cast<std::uint32_t>(
        std::clamp(width, 1.0, static_cast<double>(kMaxExciseWidth)));

    const double guard = std::round(kExciseGuardSeconds * rate);
    config.hang = static_cast<std::uint32_t>(std::clamp(
        guard, static_cast<double>(kExciseMinGuard), static_cast<double>(kExciseMaxGuard)));
    config.lead = config.hang;

    // K * S / 2, the run a burst must cover before the median moves, is at
    // least four widest impulses: S >= W / 4 and K = 32 make it 4W, plus the
    // guard segment. Sixteen samples is the floor so a segment's sum is an
    // average rather than a sample.
    const std::uint32_t quarter = std::max(16U, (config.max_width + 3U) / 4U);
    const std::uint32_t segment = std::bit_ceil(quarter);
    config.seg_log2 = std::min(static_cast<std::uint32_t>(std::countr_zero(segment)),
                               kMaxExciseSegLog2);
    config.segments = kExciseDefaultSegments;
    return config;
}

float excision_threshold(double threshold_db) {
    return static_cast<float>(std::pow(10.0, threshold_db / 10.0));
}

Status validate(const ExcisionConfig& config, const ExcisionParams& params) {
    if (config.pass > kExciseApply) {
        return fail(std::format("impulse excision: pass {} is not one of the four", config.pass));
    }
    if (config.seg_log2 < 2 || config.seg_log2 > kMaxExciseSegLog2) {
        return fail(std::format("impulse excision: a segment of 2^{} samples is outside [4, {}]",
                                config.seg_log2, 1U << kMaxExciseSegLog2));
    }
    if (config.segments < 2 || config.segments > kMaxExciseSegments) {
        return fail(std::format("impulse excision: {} reference segments is outside [2, {}]",
                                config.segments, kMaxExciseSegments));
    }
    if (config.max_width < 1 || config.max_width > kMaxExciseWidth) {
        return fail(std::format("impulse excision: a {} sample maximum width is outside [1, {}]",
                                config.max_width, kMaxExciseWidth));
    }
    if (config.hang > kExciseMaxGuard || config.lead > kExciseMaxGuard) {
        return fail(std::format("impulse excision: hang {} and lead {} must each be at most {}",
                                config.hang, config.lead, kExciseMaxGuard));
    }
    if (!is_mask(params.ring_mask) || !is_mask(params.seg_mask) || !is_mask(params.flag_mask) ||
        !is_mask(params.removed_mask)) {
        return fail("impulse excision: every mask must be a power of two minus one");
    }
    // Each ring is indexed from the sample ring's slot under its own mask,
    // which only names the same sample when it divides the sample ring.
    const std::uint64_t ring = ring_of(params.ring_mask);
    if ((ring_of(params.seg_mask) << config.seg_log2) > ring ||
        ring_of(params.flag_mask) > ring || ring_of(params.removed_mask) > ring) {
        return fail("impulse excision: the segment, flag and removed rings must each be no "
                    "larger than the sample ring they are indexed from");
    }
    const bool per_segment = config.pass == kExciseSegment || config.pass == kExciseReference;
    if (per_segment) {
        if ((params.first & (config.segment_samples() - 1U)) != 0U) {
            return fail("impulse excision: a segment pass must start on a segment boundary");
        }
        if (static_cast<std::uint64_t>(params.count) + config.segments + 2U >
            ring_of(params.seg_mask)) {
            return fail(std::format("impulse excision: {} segments and a {} segment reference do "
                                    "not fit a {} segment ring",
                                    params.count, config.segments, ring_of(params.seg_mask)));
        }
    } else {
        const std::uint64_t reach = static_cast<std::uint64_t>(params.count) +
                                    2U * config.max_width + config.hang + config.lead;
        if (reach >= ring_of(params.flag_mask) ||
            (config.pass == kExciseApply && params.count > ring_of(params.removed_mask))) {
            return fail(std::format("impulse excision: {} samples and their scan do not fit the "
                                    "flag ring of {} or the removed ring of {}",
                                    params.count, ring_of(params.flag_mask),
                                    ring_of(params.removed_mask)));
        }
    }
    if (!(params.threshold > 0.0F)) {
        return fail("impulse excision: the threshold must be a positive power ratio");
    }
    return {};
}

Expected<ExcisionRings> size_excision_rings(const ExcisionConfig& config,
                                            std::uint64_t max_block,
                                            std::uint32_t frames_in_flight,
                                            std::uint32_t ring_mask) {
    const std::uint64_t frames = std::max<std::uint32_t>(frames_in_flight, 1U);
    const std::uint64_t segment = config.segment_samples();
    const std::uint64_t lag =
        std::max<std::uint64_t>(static_cast<std::uint64_t>(config.max_width) + config.lead,
                                segment);
    const std::uint64_t in_flight = max_block * frames;

    const std::uint64_t flags = std::bit_ceil(in_flight + lag + 2U * config.max_width +
                                              config.hang + config.lead + 1U);
    const std::uint64_t segs =
        std::bit_ceil(in_flight / segment + config.segments + 4U);
    const std::uint64_t removed = std::bit_ceil((max_block + lag) * frames);

    const std::uint64_t ring = ring_of(ring_mask);
    if (flags > ring || removed > ring || (segs << config.seg_log2) > ring) {
        return fail(std::format("impulse excision: a {} sample ring is too small for {} samples "
                                "in flight and their scan",
                                ring, in_flight));
    }
    ExcisionRings out;
    out.seg_mask = static_cast<std::uint32_t>(segs - 1U);
    out.flag_mask = static_cast<std::uint32_t>(flags - 1U);
    out.removed_mask = static_cast<std::uint32_t>(removed - 1U);
    return out;
}

// ---------------------------------------------------------------------------
// The cursor
// ---------------------------------------------------------------------------

ExcisionCursor::ExcisionCursor(const ExcisionConfig& config, std::uint32_t ring_mask,
                               const ExcisionRings& rings, float threshold)
    : config_(config), ring_mask_(ring_mask), rings_(rings), threshold_(threshold) {}

ExcisionParams ExcisionCursor::params_at(SampleIndex first, std::uint32_t count) const {
    ExcisionParams out;
    out.ring_mask = ring_mask_;
    out.first = static_cast<std::uint32_t>(first & ring_mask_);
    out.count = count;
    out.seg_mask = rings_.seg_mask;
    out.flag_mask = rings_.flag_mask;
    out.removed_mask = rings_.removed_mask;
    // Clamped well inside int32: the kernels only compare it against offsets
    // a dispatch can reach, which are a block and a scan at most.
    constexpr std::int64_t kClamp = std::int64_t{1} << 30;
    const std::int64_t history =
        static_cast<std::int64_t>(first) - static_cast<std::int64_t>(valid_from_);
    out.history = static_cast<std::int32_t>(std::clamp(history, -kClamp, kClamp));
    out.threshold = threshold_;
    return out;
}

ExcisionPlan ExcisionCursor::plan(SampleIndex start, std::uint32_t count) {
    ExcisionPlan out;
    const SampleIndex segment = config_.segment_samples();

    if (!running_ || start != next_in_) {
        running_ = true;
        out.restarted = true;
        valid_from_ = start;
        seg_next_ = (start + segment - 1U) / segment;
        flag_next_ = start;
        apply_next_ = start;
    }
    const SampleIndex end = start + count;
    next_in_ = end;

    // Every segment that is now whole.
    const SampleIndex seg_end = end / segment;
    if (seg_end > seg_next_) {
        out.segment = params_at(seg_next_ * segment, static_cast<std::uint32_t>(seg_end - seg_next_));
        seg_next_ = seg_end;
    }

    if (end > flag_next_) {
        // The reference of every segment the new flags fall in. Each needs
        // the segments up to two before it, which are whole: the newest it
        // reads is (end - 1) / S - 2, below end / S.
        const SampleIndex s_lo = flag_next_ / segment;
        const SampleIndex s_hi = (end - 1U) / segment;
        out.reference = params_at(s_lo * segment, static_cast<std::uint32_t>(s_hi - s_lo + 1U));

        out.flag = params_at(flag_next_, static_cast<std::uint32_t>(end - flag_next_));
        flag_next_ = end;
    }

    // A sample is finished when the scan max_width + lead ahead of it has
    // flags, and when its own segment is whole, so no later segment pass
    // sums a sample this pass has already cleaned.
    const SampleIndex reach = static_cast<SampleIndex>(config_.max_width) + config_.lead;
    const SampleIndex by_scan = end > reach ? end - reach : 0;
    const SampleIndex by_segment = (end / segment) * segment;
    const SampleIndex apply_end = std::min(by_scan, by_segment);
    if (apply_end > apply_next_) {
        out.apply = params_at(apply_next_, static_cast<std::uint32_t>(apply_end - apply_next_));
        apply_next_ = apply_end;
    }
    out.cleaned_end = apply_next_;
    return out;
}

// ---------------------------------------------------------------------------
// The twins
// ---------------------------------------------------------------------------

Status reference_excise_segment(const ExcisionConfig& config, const ExcisionParams& params,
                                ConstComplexSpan raw, RealSpan seg_power) {
    if (auto valid = validate(config, params); !valid) {
        return valid;
    }
    if (raw.size() < ring_of(params.ring_mask) || seg_power.size() < ring_of(params.seg_mask)) {
        return fail("reference_excise_segment: a ring is shorter than its mask");
    }
    const ScopedDenormalFlush flush_denormals;
    const std::uint32_t samples = config.segment_samples();

    for (std::uint32_t j = 0; j < params.count; ++j) {
        const std::uint32_t base = (params.first + (j << config.seg_log2)) & params.ring_mask;
        // In sample order from zero, the kernel's order.
        float sum = 0.0F;
        for (std::uint32_t m = 0; m < samples; ++m) {
            const Complex32 s = raw[(base + m) & params.ring_mask];
            const float q = s.real() * s.real() + s.imag() * s.imag();
            sum = sum + q;
        }
        seg_power[(base >> config.seg_log2) & params.seg_mask] = sum;
    }
    return {};
}

Status reference_excise_reference(const ExcisionConfig& config, const ExcisionParams& params,
                                  ConstRealSpan seg_power, RealSpan seg_level) {
    if (auto valid = validate(config, params); !valid) {
        return valid;
    }
    if (seg_power.size() < ring_of(params.seg_mask) ||
        seg_level.size() < ring_of(params.seg_mask)) {
        return fail("reference_excise_reference: a segment ring is shorter than its mask");
    }
    const auto segment = static_cast<std::int64_t>(config.segment_samples());
    const auto window = static_cast<std::int64_t>(config.segments);

    for (std::uint32_t j = 0; j < params.count; ++j) {
        const std::uint32_t base = (params.first + (j << config.seg_log2)) & params.ring_mask;
        const std::uint32_t slot = base >> config.seg_log2;
        const std::int64_t rel = static_cast<std::int64_t>(j) * segment;

        // The whole window must be from this run of the stream.
        if (rel - (window + 1) * segment + params.history < 0) {
            seg_level[slot & params.seg_mask] = 0.0F;
            continue;
        }

        // Oldest first, then the kernel's insertion sort: every comparison is
        // exact, so the order and the median are the same bits on both sides.
        std::array<float, kMaxExciseSegments> v{};
        for (std::uint32_t m = 0; m < config.segments; ++m) {
            v[m] = seg_power[(slot - 1U - config.segments + m) & params.seg_mask];
        }
        for (std::uint32_t a = 1; a < config.segments; ++a) {
            const float x = v[a];
            std::uint32_t b = a;
            while (b > 0 && v[b - 1U] > x) {
                v[b] = v[b - 1U];
                --b;
            }
            v[b] = x;
        }
        seg_level[slot & params.seg_mask] = v[config.segments / 2U];
    }
    return {};
}

Status reference_excise_flag(const ExcisionConfig& config, const ExcisionParams& params,
                             ConstComplexSpan raw, ConstRealSpan seg_level,
                             std::span<std::uint32_t> flags) {
    if (auto valid = validate(config, params); !valid) {
        return valid;
    }
    if (raw.size() < ring_of(params.ring_mask) || seg_level.size() < ring_of(params.seg_mask) ||
        flags.size() < ring_of(params.flag_mask)) {
        return fail("reference_excise_flag: a ring is shorter than its mask");
    }
    const ScopedDenormalFlush flush_denormals;
    const auto samples = static_cast<float>(config.segment_samples());

    for (std::uint32_t j = 0; j < params.count; ++j) {
        const std::uint32_t slot = (params.first + j) & params.ring_mask;
        const Complex32 s = raw[slot];
        const float p = s.real() * s.real() + s.imag() * s.imag();
        const float level = seg_level[(slot >> config.seg_log2) & params.seg_mask];
        const float lhs = p * samples;
        const float rhs = params.threshold * level;
        flags[(params.first + j) & params.flag_mask] = (level > 0.0F && lhs > rhs) ? 1U : 0U;
    }
    return {};
}

Status reference_excise_apply(const ExcisionConfig& config, const ExcisionParams& params,
                              ConstComplexSpan raw, std::span<const std::uint32_t> flags,
                              ComplexSpan cleaned, ComplexSpan removed,
                              std::span<std::uint32_t> counters) {
    if (auto valid = validate(config, params); !valid) {
        return valid;
    }
    if (raw.size() < ring_of(params.ring_mask) || cleaned.size() < ring_of(params.ring_mask) ||
        flags.size() < ring_of(params.flag_mask) ||
        removed.size() < ring_of(params.removed_mask) || counters.size() < kExciseCounters) {
        return fail("reference_excise_apply: a ring is shorter than its mask");
    }

    for (std::uint32_t j = 0; j < params.count; ++j) {
        const std::int64_t rel = j;
        const std::uint32_t slot = (params.first + j) & params.ring_mask;
        const Complex32 x = raw[slot];

        bool excise = false;
        if (marked(config, params, flags, rel)) {
            std::uint32_t left = 0;
            for (std::uint32_t k = 1; k <= config.max_width; ++k) {
                if (!marked(config, params, flags, rel - k)) {
                    break;
                }
                ++left;
            }
            std::uint32_t right = 0;
            for (std::uint32_t k = 1; k <= config.max_width; ++k) {
                if (!marked(config, params, flags, rel + k)) {
                    break;
                }
                ++right;
            }
            excise = left + right + 1U <= config.max_width;
            if (left == 0U) {
                counters[excise ? kExciseCounterEvents : kExciseCounterSpared] += 1U;
            }
        }
        if (excise) {
            counters[kExciseCounterSamples] += 1U;
        }
        cleaned[slot] = excise ? kNothing : x;
        removed[(params.first + j) & params.removed_mask] = excise ? x : kNothing;
    }
    return {};
}

}  // namespace revenant::dsp
