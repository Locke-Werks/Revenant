// Span-wide impulse excision: removing impulse noise from the whole capture
// before the channelizer, so the spectrum, the waterfall, the detector and
// every receiver see the cleaned stream.
//
// WHY A SECOND BLANKER. core/dsp/noise_reference.h already has one, per
// receiver, on its coarse channel. That one only helps the receiver that
// switched it on, and only after the channelizer's prototype filter has
// smeared each impulse over 2 * taps_per_branch channel samples. Ignition,
// switching supplies and lightning are wideband: one impulse lights up every
// bin of the waterfall and raises the detector's floor everywhere at once.
// At the source rate the same impulse is a handful of samples, so cutting it
// out there costs a handful of samples of programme and fixes every consumer
// in one place.
//
// WHERE IT RUNS. On the device ring, in place, after the front-end
// correction (core/shaders/iq_correct.comp) and before the channelizer
// (core/shaders/pfb_branch.comp). The DC removal goes first because an offset
// is a constant added to every sample, and a constant added to an impulse
// changes its height against the reference; correcting it first means the
// detector sees what the antenna delivered.
//
// THE DETECTOR, designed from the same detection-by-threshold principle as
// the receiver blanker (S. V. Vaseghi, "Advanced Digital Signal Processing
// and Noise Reduction", Wiley, the chapter "Impulsive Noise: Modelling,
// Detection and Removal", cited by chapter title and not opened for this
// work; core/dsp/noise_reference.h says the same of its own design):
//
//   segment    p[s] = sum of |x|^2 over the S samples of segment s, S a power
//              of two, segments aligned to the absolute sample index.
//
//   reference  r[s] = the median of p over the K segments that end one
//              segment before s, so the segment immediately before is a
//              guard. A MEDIAN, not a mean, and that is what keeps the
//              stage off real signals: a burst raises a mean reference by
//              its power times its length over the window, so with a mean a
//              burst stops standing out after W / T samples and its first
//              W / T samples look like an impulse. The median does not move
//              until the burst covers half the window, so a burst stays
//              flagged for K * S / 2 samples, which the design keeps at four
//              times the widest impulse. A strong continuous carrier is in
//              every segment, so it is in the reference, and a constant
//              envelope never stands 15 dB above itself.
//
//   flag       f[i] = 1 when |x[i]|^2 * S > T * r[seg(i)], written as two
//              products compared rather than a division for the reason
//              core/shaders/noise_blank.comp gives. A zero reference never
//              flags, which is how a restart waits for a full window.
//
//   apply      a flag at f marks [f - lead, f + hang] as part of an event.
//              An event is a maximal run of marked samples. A run of at most
//              max_width samples is an impulse and is excised. A longer run
//              is a signal, a burst of somebody's transmission, and is left
//              exactly as it was.
//
// EXCISED, NOT INTERPOLATED, NOT TAPERED, NOT SHIFTED. An excised sample is
// replaced by zero and keeps its index: the sample index is time
// (docs/conventions.md, "Time is a sample index"), and nothing downstream can
// tell a moved sample from a frequency error. Zero rather than a raised-
// cosine taper, for three reasons, measured where a number exists:
//
//   - A taper keeps part of the impulse. The impulse is the thing standing
//     15 dB or more above the whole span's median power, and the taper's
//     shoulders let w(n) of it through on each side, where w rises to one.
//     What leaks is broadband by construction, which is exactly what the
//     stage exists to keep off the waterfall.
//   - The gate's own splatter is small at these widths. Zeroing N samples of
//     a carrier of power P subtracts a burst of energy N * P from it, and
//     that burst's spectrum is a sinc fs / N wide. At N = 5 out of a
//     65536-sample block that is 5 / 65536 of the carrier's energy spread
//     over most of the span, 39 dB under the carrier in total and far below
//     the floor in any one bin. tests/reference/test_impulse_excision.cpp
//     holds a tone's power to 0.1 dB with impulses excised around it.
//   - Zero is what makes the audit exact. See "Raw plus removed" below. A
//     taper's removed part is x - w * x, which does not add back to x in
//     floating point.
//
// RAW PLUS REMOVED. The apply pass writes the removed component beside the
// cleaned one, into a ring of its own, so cleaned + removed == raw bit for
// bit for every sample the pass covers. The trick is the sign of the zero:
// the side that carries nothing carries NEGATIVE zero, because -0 + x is x
// for every float x including +0 and -0, where +0 + -0 would be +0 and lose
// a raw negative zero. Downstream, -0 is zero. Nothing reads the removed ring
// yet; it exists so a tap that ships it to a client is a copy out of a buffer
// that is already correct, not a change to the stage.
//
// A LAG, AND WHO PAYS IT. The apply pass decides a sample's run length by
// looking max_width + lead samples ahead, so it can only finish what is that
// far behind the newest sample. The graph holds the channelizer back to what
// the stage has finished (ExcisionPlan::cleaned_end), which at the defaults
// is 402 samples plus up to one segment, about 20 us at 20 MS/s, against a
// 13.6 ms block. Nothing else
// waits. The last lag's worth of samples in a finite capture is never handed
// to the channelizer, the same way the channelizer's own filter support is
// never handed to anything at the start.
//
// PROVENANCE, per docs/clean-room.md. The design is this project's own from
// the published principle above. No GPL implementation was read, fetched or
// consulted. Every constant below is stated with the reason it has its value.

#pragma once

#include <cstdint>
#include <span>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::dsp {

// The passes, kPass at specialization constant id 1 of
// core/shaders/impulse_excise.comp. One kernel file, four pipelines.
inline constexpr std::uint32_t kExciseSegment = 0;
inline constexpr std::uint32_t kExciseReference = 1;
inline constexpr std::uint32_t kExciseFlag = 2;
inline constexpr std::uint32_t kExciseApply = 3;

// The kernel holds the reference window in a private array of this many
// floats, so K cannot be larger.
inline constexpr std::uint32_t kMaxExciseSegments = 64;
inline constexpr std::uint32_t kMaxExciseWidth = 4096;
inline constexpr std::uint32_t kMaxExciseSegLog2 = 12;

// The counters the apply pass adds to, in this order.
inline constexpr std::uint32_t kExciseCounterSamples = 0;
inline constexpr std::uint32_t kExciseCounterEvents = 1;
inline constexpr std::uint32_t kExciseCounterSpared = 2;
inline constexpr std::uint32_t kExciseCounters = 4;

// The defaults, in the units an operator thinks in.
//
//   threshold  15 dB. Complex Gaussian noise stands 15 dB over its mean
//              power with probability exp(-31.6), about 2e-14, so at 20 MS/s
//              noise alone flags about once a month. The receiver
//              blanker's 12 dB would flag 2.6 times a second at that rate,
//              each time cutting a handful of samples out of every
//              receiver at once, and at the source rate an impulse stands
//              far higher than 15 dB above the whole span's median anyway:
//              its energy is in a few samples rather than smeared.
//   max width  20 us. Ignition and switching impulses are well under 10 us
//              at a wideband front end; the shortest real bursts the
//              detector is asked to find, a P25 or DMR slot's ramp and an
//              ADS-B preamble, are 8 us and up and are carried by
//              transmissions far longer than this. A run longer than this
//              is left alone.
//   hang/lead  0.1 us either side, at least two samples. The device's own
//              anti-alias filter spreads an impulse over a few samples, and
//              the skirts below the threshold are still the impulse.
inline constexpr double kExciseDefaultThresholdDb = 15.0;
inline constexpr double kExciseDefaultMaxWidthSeconds = 20e-6;
inline constexpr double kExciseGuardSeconds = 0.1e-6;
inline constexpr std::uint32_t kExciseMinGuard = 2;
inline constexpr std::uint32_t kExciseMaxGuard = 16;
inline constexpr std::uint32_t kExciseDefaultSegments = 32;

// Specialization constants of core/shaders/impulse_excise.comp at ids 1 to 6.
struct ExcisionConfig {
    std::uint32_t pass = kExciseSegment;
    std::uint32_t seg_log2 = 6;
    std::uint32_t segments = kExciseDefaultSegments;
    std::uint32_t max_width = 400;
    std::uint32_t hang = 2;
    std::uint32_t lead = 2;

    [[nodiscard]] std::uint32_t segment_samples() const { return 1U << seg_log2; }

    friend constexpr bool operator==(const ExcisionConfig&, const ExcisionConfig&) = default;
};

// Eight packed words, the push constant block of all four passes.
//
// `first` is the ring slot of the dispatch's first sample, (absolute index &
// ring_mask); for the segment and reference passes it is the first sample of
// the first segment, so a multiple of S. Every other ring is indexed from the
// same slot under its own mask, which is the same as indexing it by the
// absolute index because each of them is a power of two no larger than the
// sample ring. That is checked by validate rather than assumed.
//
// `history` is how many samples of the current run of the stream precede
// `first`: the absolute index of `first` minus the index the stage last
// restarted at. Negative when the dispatch starts before the restart, which
// a reference pass does when the restart was not on a segment boundary. A
// flag or a segment from before the restart is stale ring and is never read.
struct ExcisionParams {
    std::uint32_t ring_mask = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::uint32_t seg_mask = 0;
    std::uint32_t flag_mask = 0;
    std::uint32_t removed_mask = 0;
    std::int32_t history = 0;

    // T as a power ratio, 10^(dB/10) rounded to float once on the host.
    float threshold = 31.622776F;
};

static_assert(sizeof(ExcisionParams) == 8 * sizeof(std::uint32_t),
              "ExcisionParams must be eight packed 32-bit words to alias the kernel's push "
              "constant block");

// The figures above for one source rate. max_width_seconds is clamped to
// what the kernel's loops are sized for.
[[nodiscard]] ExcisionConfig design_excision(
    SampleRate source_rate, double max_width_seconds = kExciseDefaultMaxWidthSeconds);
[[nodiscard]] float excision_threshold(double threshold_db);

[[nodiscard]] Status validate(const ExcisionConfig& config, const ExcisionParams& params);

// The rings the stage needs for a given largest block, each a power of two:
// flags must reach back over the scan behind the oldest sample one apply
// pass finishes and forward to the newest flag, segments over one block plus
// the reference window, and the removed ring over one block plus the lag for
// each frame in flight, so frames overlapping on the device write disjoint
// slots.
struct ExcisionRings {
    std::uint32_t seg_mask = 0;
    std::uint32_t flag_mask = 0;
    std::uint32_t removed_mask = 0;
};
[[nodiscard]] Expected<ExcisionRings> size_excision_rings(const ExcisionConfig& config,
                                                          std::uint64_t max_block,
                                                          std::uint32_t frames_in_flight,
                                                          std::uint32_t ring_mask);

// One block's four dispatches. A count of zero is a pass with nothing to do.
struct ExcisionPlan {
    ExcisionParams segment{};
    ExcisionParams reference{};
    ExcisionParams flag{};
    ExcisionParams apply{};

    // One past the last sample the stage has finished. Everything below it,
    // back to the last restart, is cleaned in place; the channelizer may read
    // up to here and no further.
    SampleIndex cleaned_end = 0;

    // True when this block started a new run: the first block, or one that
    // did not follow on from the last.
    bool restarted = false;
};

// The host side of the stage: which samples each pass covers this block.
// Pure bookkeeping on absolute indices, shared by the graph and by the
// behaviour tests' CPU run so both drive the kernels identically.
class ExcisionCursor {
public:
    ExcisionCursor() = default;
    ExcisionCursor(const ExcisionConfig& config, std::uint32_t ring_mask,
                   const ExcisionRings& rings, float threshold);

    // Forgets everything. The next block restarts the stage, which then
    // waits for a full reference window before it flags anything.
    void reset() { running_ = false; }

    // The block [start, start + count) has just been written to the ring.
    [[nodiscard]] ExcisionPlan plan(SampleIndex start, std::uint32_t count);

    [[nodiscard]] SampleIndex cleaned_end() const { return apply_next_; }
    [[nodiscard]] const ExcisionConfig& config() const { return config_; }

private:
    [[nodiscard]] ExcisionParams params_at(SampleIndex first, std::uint32_t count) const;

    ExcisionConfig config_{};
    std::uint32_t ring_mask_ = 0;
    ExcisionRings rings_{};
    float threshold_ = 0.0F;

    bool running_ = false;
    SampleIndex valid_from_ = 0;
    SampleIndex next_in_ = 0;
    SampleIndex seg_next_ = 0;
    SampleIndex flag_next_ = 0;
    SampleIndex apply_next_ = 0;
};

// Twins of the four passes. Each takes every ring whole, as the kernel's
// bindings see them, and writes only what its pass writes:
//
//   segment    seg_power at the segments' slots
//   reference  seg_level at the segments' slots
//   flag       flags at the samples' slots
//   apply      cleaned and removed at the samples' slots, and adds to
//              counters
//
// `raw` and `cleaned` are separate so a conformance test can compare them;
// the engine binds the one ring as both, which is safe because the apply
// pass reads and writes only its own slot of either.
[[nodiscard]] Status reference_excise_segment(const ExcisionConfig& config,
                                              const ExcisionParams& params, ConstComplexSpan raw,
                                              RealSpan seg_power);
[[nodiscard]] Status reference_excise_reference(const ExcisionConfig& config,
                                                const ExcisionParams& params,
                                                ConstRealSpan seg_power, RealSpan seg_level);
[[nodiscard]] Status reference_excise_flag(const ExcisionConfig& config,
                                           const ExcisionParams& params, ConstComplexSpan raw,
                                           ConstRealSpan seg_level,
                                           std::span<std::uint32_t> flags);
[[nodiscard]] Status reference_excise_apply(const ExcisionConfig& config,
                                            const ExcisionParams& params, ConstComplexSpan raw,
                                            std::span<const std::uint32_t> flags,
                                            ComplexSpan cleaned, ComplexSpan removed,
                                            std::span<std::uint32_t> counters);

}  // namespace revenant::dsp
