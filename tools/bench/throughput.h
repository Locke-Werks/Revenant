// What the architecture has been asserting and nobody has measured.
//
// Three claims in this repository are load-bearing and were, until this file,
// unmeasured:
//
//   1. "Two hundred receivers cost what two cost." M1's exit criterion is
//      fifty simultaneous receivers.
//   2. docs/fft.md records VkFFT at 790 to 800 GB/s on the 4090 and sets a
//      standing reopen condition: "If the kernel lands below about 60% of
//      that, this decision is worth reopening."
//   3. core/engine/vrx_stage.cpp predicts that the per-receiver seam costs 2N
//      global pipeline barriers per block and that no two receivers overlap,
//      naming fifty receivers as where it starts to matter.
//
// This header declares the measurement, not an opinion about the result. If a
// number comes out bad it is reported; a harness that prints good news it did
// not measure is worse than no harness.
//
// THREE MEASUREMENTS, AND WHY THEY ARE SEPARATE
//
// End to end. A real Engine over a real Source, receivers added, wall-clocked
// around Engine::run(). This is the only figure that includes the host: the
// staging memcpy, the command recording, the completion thread's readback and
// RMS per receiver per block. It is what "fifty receivers work" has to mean.
//
// The source ceiling. The same scene driven with a sink that counts bytes and
// nothing else. Without it the end-to-end curve is uninterpretable, and the
// reason is not hypothetical:
//
//   --emitters 8 --noise     source 7.35 MS/s, engine 0.353x at 0 receivers
//                            and 0.351x at 8. Every row is the generator.
//   silent, the default      source 3005 MS/s, engine 34.99x at 0 receivers
//                            and 1.57x at 100. Every row is the engine.
//
// Both measured on the 4090 at 20 MS/s on 2026-09-18. The scene generator is
// single threaded and a populated scene at this rate costs about four hundred
// times what a silent one does, so the default scene is silent and the
// ceiling is measured and printed first rather than asserted.
//
// The device rig. The coarse chain and N receiver stages recorded into one
// command buffer with GPU timestamp queries, submitted on its own. This is
// where the per-stage split and the channelizer's bandwidth come from,
// because they are not visible from the outside of a run.
//
// WHY THE RIG DOES NOT PERTURB WHAT IT MEASURES
//
// Every timestamp sits at a point where core/engine/graph.cpp already records
// a pipeline barrier: after the branch filter, after the transform, and after
// the last receiver's stage. A timestamp written at a point that is already a
// full serialisation costs no additional serialisation, so the split is a
// reading of the real schedule rather than one the measurement imposed. A
// timestamp between two receivers would not have that property, which is why
// there is none: the per-receiver figure here is the region divided by N, and
// the question of whether receivers overlap is answered by how that figure
// moves with N rather than by forcing them apart to look.
//
// The rig records the coarse dispatches itself, because the graph's are
// inside a private implementation. It asks the installed VrxStage factory for
// the receivers, so the fine stage, the detector and their 2N barriers are
// the engine's own code and not a copy of it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/engine/vrx.h"
#include "core/error.h"

namespace revenant::bench {

// Bumped when the JSON shape changes, for the same reason the curve file
// carries one: a reader that misreads a field silently is worse than one that
// refuses.
inline constexpr int kThroughputSchemaVersion = 1;

// The bar docs/fft.md sets and the line it says to reopen the decision at.
// Named here rather than written into a format string so the two places that
// print them cannot drift apart.
inline constexpr double kVkFftGigabytesPerSecond = 790.0;
inline constexpr double kReopenFraction = 0.60;

struct ThroughputConfig {
    // -1 honours REVENANT_GPU_INDEX, which is how the conformance matrix aims
    // every other binary in this repository.
    int gpu_index = -1;

    dsp::SampleRate rate = 20'000'000;
    std::uint32_t channels = 64;
    std::size_t block_samples = 65'536;
    dsp::SampleRate audio_rate = 48'000;

    // Seconds of source time per end-to-end point. Engine::run() polls its
    // stop condition every 2 ms, so a point that finishes in a few
    // milliseconds is mostly measuring that poll. Four seconds of signal at
    // 20 MS/s is 611 blocks and lands every point well clear of it.
    double seconds = 4.0;

    std::uint64_t seed = 20'260'918;

    // The scene is silent by default and that is a measurement decision, not
    // laziness. See the source-ceiling note above: a populated scene makes
    // every row of the curve read the generator's rate. Raise these and the
    // ceiling row shows it happening.
    std::size_t emitters = 0;
    bool noise = false;

    engine::Demod demod = engine::Demod::Nfm;
    dsp::Hertz bandwidth = 16'000;

    std::vector<std::uint32_t> receiver_counts{0, 1, 2, 8, 16, 32, 50, 100};

    // Device rig. Iterations after the warmup are what the median is taken
    // over; the warmup exists because a receiver's first recorded block also
    // uploads its tap table.
    bool device_rig = true;
    std::uint32_t iterations = 32;
    std::uint32_t warmup = 4;

    // The channelizer bandwidth pass. 128 MiB is VkFFT's own test size in
    // docs/fft.md, so the two numbers are comparable without a correction.
    bool bandwidth_pass = true;
    std::uint64_t transform_bytes = 128ULL << 20;
    std::uint32_t bandwidth_iterations = 16;
    std::vector<std::uint32_t> transform_sizes{64, 128, 256, 1024, 2048};

    [[nodiscard]] Status validate() const;
};

// The scene alone, with a sink that counts and does nothing else.
struct SourceCeiling {
    std::string uri;
    std::uint64_t samples = 0;
    std::uint64_t blocks = 0;
    double wall_seconds = 0.0;

    [[nodiscard]] double samples_per_second() const;
    [[nodiscard]] double realtime_ratio(dsp::SampleRate rate) const;
};

// One receiver count, measured through the whole engine.
struct EndToEndPoint {
    std::uint32_t receivers = 0;
    double wall_seconds = 0.0;
    std::uint64_t blocks = 0;
    std::uint64_t samples = 0;
    std::uint64_t audio_frames = 0;
    std::uint64_t overrun_events = 0;
    std::uint64_t samples_dropped = 0;

    [[nodiscard]] double blocks_per_second() const;
    [[nodiscard]] double samples_per_second() const;
    [[nodiscard]] double realtime_ratio(dsp::SampleRate rate) const;
    [[nodiscard]] double microseconds_per_block() const;
};

// One receiver count, measured on the device with timestamp queries. Every
// figure is a median over the rig's iterations, in microseconds of GPU time.
struct DevicePoint {
    std::uint32_t receivers = 0;
    std::uint32_t blocks = 0;

    double branch_us = 0.0;
    double fft_us = 0.0;
    double receivers_us = 0.0;
    double total_us = 0.0;

    // 2N global memory barriers and nothing between them, submitted on their
    // own. The control for the prediction in core/engine/vrx_stage.cpp: it
    // says what 2N barriers cost on this device, so the per-receiver figure
    // can be split into barrier and work rather than argued about.
    double barrier_control_us = 0.0;
};

// The channelizer's transform, sized to match VkFFT's own test.
struct BandwidthPoint {
    std::uint32_t channels = 0;
    std::uint32_t decimation = 0;
    std::uint32_t blocks = 0;

    double fft_us = 0.0;
    double branch_us = 0.0;

    // The fastest iteration. Carried beside the median because at the largest
    // transform the two disagree by a third from one run of the tool to the
    // next, and a single number would hide that the figure is not settled.
    double fft_us_best = 0.0;
    double branch_us_best = 0.0;

    // Counted, not estimated. See docs and the printed arithmetic: the FFT
    // reads the branch scratch once and writes the channel ring once, so the
    // traffic it is fair to divide by its time is exactly those two.
    std::uint64_t fft_bytes_read = 0;
    std::uint64_t fft_bytes_written = 0;

    // The branch filter gathers kTapsPerBranch samples per output at a stride
    // of M, so what it issues and what DRAM has to supply are different
    // numbers by a large factor. Both are carried, because quoting either one
    // alone would be a GB/s figure nobody can check.
    std::uint64_t branch_bytes_issued = 0;
    std::uint64_t branch_bytes_unique = 0;
    std::uint64_t branch_bytes_written = 0;

    [[nodiscard]] double fft_gigabytes_per_second() const;
    [[nodiscard]] double fft_best_gigabytes_per_second() const;
    [[nodiscard]] double branch_issued_gigabytes_per_second() const;
    [[nodiscard]] double branch_unique_gigabytes_per_second() const;
};

struct ThroughputReport {
    ThroughputConfig config;

    std::string device_name;
    std::string device_summary;
    dsp::GridParams grid;
    dsp::SampleRate channel_rate = 0;

    // False when the queue family reports no valid timestamp bits. Every
    // device figure is then absent rather than wrong, and the printed report
    // says so instead of showing zeros.
    bool timestamps_supported = false;
    double timestamp_period_ns = 0.0;

    SourceCeiling ceiling;
    std::vector<EndToEndPoint> end_to_end;
    std::vector<DevicePoint> device;
    std::vector<BandwidthPoint> bandwidth;

    // Anything the run had to shrink to fit, in the words the shrinking code
    // used. Printed, never swallowed.
    std::vector<std::string> notes;
};

// Runs the whole measurement. Long: the end-to-end pass alone opens one engine
// per receiver count.
[[nodiscard]] Expected<ThroughputReport> run_throughput(const ThroughputConfig& config);

// The source URI this config measures against, built once so the ceiling pass
// and every engine pass are demonstrably the same scene.
[[nodiscard]] std::string throughput_source_uri(const ThroughputConfig& config);

[[nodiscard]] std::string throughput_to_json(const ThroughputReport& report);

// Prints the tables a person reads. Separate from the JSON because they are
// not the same document: the table carries the arithmetic behind each derived
// number and the JSON carries the inputs to it.
void print_throughput(const ThroughputReport& report);

}  // namespace revenant::bench
