// The spectrum's percentile stage, GPU against CPU, bit for bit, plus the
// checks bit-exactness cannot make.
//
// Three kinds of case, and they catch different things.
//
//   The host cases referee the referee. A kernel and its twin can agree
//   perfectly on a number that is not the percentile of anything, so the
//   histogram's answer is checked against an exact sort of the same data and
//   the bucket mapping is walked across a boundary by hand. This is the same
//   role tests/reference/test_spectrum.cpp's logarithm case plays for
//   det_log2.
//
//   The bit-exact cases run the kernel against its twin and demand identical
//   bits, at several workgroup widths and on frames built to put mass on a
//   bucket boundary. They are what the conformance matrix runs on every
//   device.
//
//   The shape cases cover what a random frame does not reach: a frame whose
//   content is uniform, a frame that is all silence, and a frame with one
//   spur in it, which is the case docs/ui-spectrum.md names as the reason
//   this is a percentile rather than a maximum.
//
// WHY THE BIT-EXACT CASES CARRY THE SHARED-MEMORY GATE
//
// This kernel holds a histogram rather than a transform in workgroup shared
// memory, so the fault docs/fft.md records is not obviously its fault to
// catch: that one is a transform miscompiled after several differing
// dispatches, and integer atomics into a 4 KiB array are a long way from it.
//
// The gate is here anyway, and the reasoning is about which way to be wrong.
// This kernel runs in the same process as the spectrum kernel, in the same
// binary, against the same device, and its shared memory is shared memory.
// Leaving it ungated on a device that is known to return different bits for
// the same shared-memory dispatch would make a red build that means nothing
// on one device while the green on the other means something. The cost of
// gating is a skip on the integrated part, which is visible; the cost of not
// gating is a failure nobody can act on, which is worse. docs/fft.md carries
// the measurement and what would settle it.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/pfb_fft_reference.h"
#include "core/dsp/spectrum_levels_reference.h"
#include "core/dsp/spectrum_reference.h"
#include "core/dsp/types.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;
using Catch::Approx;

namespace {

// The frame the engine actually produces: 64 coarse channels at 2048 points,
// central half kept. Every case that can afford it runs this size, because
// the one thing tests/reference/test_spectrum.cpp learned the hard way is
// that a suite which never dispatches the shipped shape is a suite that can
// pass while the product is broken.
constexpr std::uint32_t kEngineBins = 64U * (2048U / 2U);

// Mirrors the push constant block of core/shaders/spectrum_levels.comp. Kept
// separate from dsp::SpectrumLevelsParams for the same reason the spectrum
// test keeps its own: one is what validate() checks and the other is what
// goes on the wire.
struct LevelsPush {
    std::uint32_t bins = 0;
    std::uint32_t low_permille = 0;
    std::uint32_t high_permille = 0;
};

dsp::SpectrumLevelsParams make_params(std::uint32_t bins) {
    dsp::SpectrumLevelsParams params;
    params.bins = bins;
    params.low_permille = dsp::kSpectrumLowPermille;
    params.high_permille = dsp::kSpectrumHighPermille;
    return params;
}

std::vector<float> run_levels_on_gpu(const dsp::SpectrumLevelsParams& params,
                                     std::span<const float> frame,
                                     std::uint32_t local_size) {
    auto& context = test::shared_context();

    std::vector<float> out(dsp::kSpectrumLevelsOutputs, 0.0F);

    const LevelsPush push{
        .bins = params.bins,
        .low_permille = params.low_permille,
        .high_permille = params.high_permille,
    };

    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::spectrum_levels();
    invocation.inputs = {std::as_bytes(frame)};
    invocation.outputs = {std::as_writable_bytes(std::span<float>(out))};
    invocation.push_constants = std::as_bytes(std::span<const LevelsPush>(&push, 1));
    invocation.local_size_x = local_size;
    // One workgroup, always. The kernel holds the whole histogram in shared
    // memory and a second group would count the same bins into its own copy.
    invocation.group_count_x = 1;
    invocation.invocations = local_size;

    const auto ran = gpu::run_kernel(context, invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
    return out;
}

std::vector<float> run_levels_on_cpu(const dsp::SpectrumLevelsParams& params,
                                     std::span<const float> frame) {
    std::vector<float> out(dsp::kSpectrumLevelsOutputs, 0.0F);
    const auto computed = dsp::reference_spectrum_levels(params, frame, out);
    INFO(test::message_of(computed));
    REQUIRE(computed.has_value());
    return out;
}

// The exact order statistic the histogram is approximating, by sorting.
// Slower than the thing under test by orders of magnitude and correct by
// construction, which is the only property wanted of it.
float exact_percentile(std::span<const float> frame, std::uint32_t permille) {
    std::vector<float> sorted(frame.begin(), frame.end());
    std::sort(sorted.begin(), sorted.end());

    const auto count = static_cast<std::uint64_t>(sorted.size());
    auto rank = static_cast<std::size_t>(count * permille / 1000ULL);
    rank = std::min(rank, sorted.size() - 1);
    return sorted[rank];
}

// A frame shaped like a real one: a noise floor with a few signals standing
// out of it. Deterministic from the seed, per docs/conventions.md.
std::vector<float> synthetic_frame(std::uint64_t seed, std::uint32_t bins, float floor_db,
                                   float spread_db, std::size_t signals,
                                   float signal_db) {
    test::SeededInput input(seed);
    std::vector<float> frame = input.reals(bins, floor_db, floor_db + spread_db);
    for (std::size_t i = 0; i < signals; ++i) {
        frame[(i * 977U + 13U) % bins] = signal_db;
    }
    return frame;
}

}  // namespace

// ---------------------------------------------------------------------------
// The host cases: is it a percentile at all
// ---------------------------------------------------------------------------

TEST_CASE("the histogram bucket mapping is exact at its boundaries", "[spectrum][levels][m1]") {
    // Bit-exactness against a twin says the device buckets what the twin
    // buckets. It says nothing about whether either buckets correctly, and a
    // value landing on the wrong side of a boundary is the whole of what can
    // go wrong here.

    CHECK(dsp::spectrum_levels_bucket_of(dsp::kSpectrumLevelsRangeFloorDb) == 0);

    // Everything below the range floors into bucket zero rather than wrapping
    // to the top, which an unclamped conversion would do.
    CHECK(dsp::spectrum_levels_bucket_of(-1000.0F) == 0);
    CHECK(dsp::spectrum_levels_bucket_of(dsp::kSpectrumLevelsRangeCeilingDb) ==
          dsp::kSpectrumLevelsBuckets - 1);
    CHECK(dsp::spectrum_levels_bucket_of(1000.0F) == dsp::kSpectrumLevelsBuckets - 1);

    // Each bucket's own centre has to come back to it, and the value reported
    // for a bucket has to be inside the bucket. Walked across every boundary
    // rather than sampled, because there are only a thousand of them.
    for (std::uint32_t b = 0; b < dsp::kSpectrumLevelsBuckets; ++b) {
        const float centre = dsp::spectrum_levels_value_of(b);
        INFO("bucket " << b << " has centre " << centre);
        CHECK(dsp::spectrum_levels_bucket_of(centre) == b);

        const float lower = dsp::kSpectrumLevelsRangeFloorDb +
                            static_cast<float>(b) * dsp::kSpectrumLevelsDbPerBucket;
        CHECK(centre > lower);
        CHECK(centre < lower + dsp::kSpectrumLevelsDbPerBucket);
    }

    // The reported value is a bucket centre, so the worst it can be wrong by
    // is half a bucket. This is the error figure the header states and it is
    // checked here rather than asserted there.
    CHECK(dsp::kSpectrumLevelsDbPerBucket * 0.5F == Approx(0.1171875).margin(1.0e-9));
}

TEST_CASE("the histogram agrees with an exact sort to within half a bucket",
          "[spectrum][levels][m1]") {
    // The claim the whole approach rests on. If this fails, the kernel and
    // its twin can still be bit-identical and the number they agree on is
    // not a percentile.
    constexpr std::uint64_t kSeed = 0x4C564C5300000001ULL;

    struct Shape {
        const char* what;
        float floor_db;
        float spread_db;
        std::size_t signals;
        float signal_db;
    };

    const Shape shapes[] = {
        {"a flat noise floor", -100.0F, 6.0F, 0, 0.0F},
        {"noise with a handful of carriers", -110.0F, 12.0F, 64, -20.0F},
        {"a wide distribution across most of the range", -180.0F, 170.0F, 0, 0.0F},
        {"a band close to full scale", -8.0F, 8.0F, 0, 0.0F},
    };

    for (const Shape& shape : shapes) {
        const auto frame = synthetic_frame(kSeed, kEngineBins, shape.floor_db, shape.spread_db,
                                           shape.signals, shape.signal_db);
        const auto params = make_params(kEngineBins);
        const auto measured = run_levels_on_cpu(params, frame);

        const float exact_low = exact_percentile(frame, params.low_permille);
        const float exact_high = exact_percentile(frame, params.high_permille);

        INFO(std::format("{}: low {} against exact {}, high {} against exact {}", shape.what,
                         measured[0], exact_low, measured[1], exact_high));

        // Half a bucket for the value quantisation, plus one whole bucket of
        // slack for the rank: the histogram's rank and the sort's rank can
        // name adjacent order statistics, and on a continuous distribution
        // those sit under a bucket apart. A bug puts the answer decibels out,
        // not fractions of one.
        const double tolerance = 1.5 * static_cast<double>(dsp::kSpectrumLevelsDbPerBucket);
        CHECK(std::abs(static_cast<double>(measured[0] - exact_low)) <= tolerance);
        CHECK(std::abs(static_cast<double>(measured[1] - exact_high)) <= tolerance);
    }
}

TEST_CASE("a single spur does not move the high percentile", "[spectrum][levels][m1]") {
    // docs/ui-spectrum.md's reason for percentiles: "The minimum and maximum
    // of a frame are a dead bin and a spur." This is that sentence as a test.
    constexpr std::uint64_t kSeed = 0x4C564C5300000002ULL;

    auto frame = synthetic_frame(kSeed, kEngineBins, -100.0F, 10.0F, 0, 0.0F);
    const auto params = make_params(kEngineBins);
    const auto clean = run_levels_on_cpu(params, frame);

    // One bin at full scale and one stuck at the silence floor, which is what
    // a maximum and a minimum would each follow all the way.
    frame[frame.size() / 3] = 0.0F;
    frame[frame.size() / 7] = dsp::kSpectrumFloorDb;
    const auto spoiled = run_levels_on_cpu(params, frame);

    INFO(std::format("clean {} to {}, with one spur and one dead bin {} to {}", clean[0],
                     clean[1], spoiled[0], spoiled[1]));

    // Two bins out of sixty-five thousand cannot move a percentile by more
    // than the bucket it was already quantised into.
    CHECK(spoiled[0] == Approx(clean[0]).margin(dsp::kSpectrumLevelsDbPerBucket));
    CHECK(spoiled[1] == Approx(clean[1]).margin(dsp::kSpectrumLevelsDbPerBucket));

    // And the spur is still ninety decibels above the ceiling the map would
    // have used, which is the point: it clips rather than rescaling the
    // display around itself.
    CHECK(0.0F - spoiled[1] > 80.0F);
}

TEST_CASE("a frame whose content is uniform reports a span of nothing",
          "[spectrum][levels][m1]") {
    // The measurement half of the no-saturation requirement. Every bin
    // identical means both percentiles land in the same bucket and the span
    // is zero, which is correct and is why SpectrumScale carries a minimum
    // span; the display half of the requirement is in
    // tests/engine/test_spectrum_scale.cpp.
    const std::vector<float> frame(kEngineBins, -73.25F);
    const auto measured = run_levels_on_cpu(make_params(kEngineBins), frame);

    INFO(std::format("a uniform frame at -73.25 dB reports {} to {}", measured[0], measured[1]));
    CHECK(measured[0] == measured[1]);
    CHECK(measured[0] == Approx(-73.25).margin(dsp::kSpectrumLevelsDbPerBucket));
}

TEST_CASE("a silent frame reports the silence floor", "[spectrum][levels][m1]") {
    // What the stage does before a source has delivered anything, and on a
    // band with the antenna disconnected. The spectrum kernel's own floor is
    // the bottom of this histogram's range exactly, so it must not clamp into
    // bucket zero from below and must not read as anything but silence.
    const std::vector<float> frame(kEngineBins, dsp::kSpectrumFloorDb);
    const auto measured = run_levels_on_cpu(make_params(kEngineBins), frame);

    INFO(std::format("a silent frame reports {} to {}", measured[0], measured[1]));
    CHECK(measured[0] == Approx(dsp::kSpectrumFloorDb).margin(dsp::kSpectrumLevelsDbPerBucket));
    CHECK(measured[1] == Approx(dsp::kSpectrumFloorDb).margin(dsp::kSpectrumLevelsDbPerBucket));
}

TEST_CASE("the levels stage refuses a parameter set it cannot serve",
          "[spectrum][levels][m1]") {
    auto params = make_params(kEngineBins);
    CHECK(dsp::validate(params).has_value());

    params.bins = 0;
    CHECK_FALSE(dsp::validate(params).has_value());

    params = make_params(kEngineBins);
    params.low_permille = params.high_permille;
    CHECK_FALSE(dsp::validate(params).has_value());

    params = make_params(kEngineBins);
    params.high_permille = 1001;
    CHECK_FALSE(dsp::validate(params).has_value());
}

// ---------------------------------------------------------------------------
// Bit-exact against the twin
// ---------------------------------------------------------------------------

TEST_CASE("the levels stage matches its CPU twin bit-exactly", "[gpu][spectrum][levels][m1]") {
    REVENANT_NEEDS_GPU();
    REVENANT_NEEDS_REPRODUCIBLE_SHARED_MEMORY();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x4C564C5300000003ULL;

    const auto frame = synthetic_frame(kSeed, kEngineBins, -110.0F, 30.0F, 128, -12.0F);
    const auto params = make_params(kEngineBins);

    const auto gpu_result = run_levels_on_gpu(params, frame, 256);
    const auto cpu_result = run_levels_on_cpu(params, frame);

    // A diff of two buffers that are both the range floor passes, and so does
    // a diff of two kernels that both did nothing. Nothing here should be at
    // the bottom of the range: the frame's own floor is -110.
    INFO(std::format("gpu {} to {}, cpu {} to {}", gpu_result[0], gpu_result[1], cpu_result[0],
                     cpu_result[1]));
    REQUIRE(cpu_result[0] > dsp::kSpectrumLevelsRangeFloorDb + 1.0F);
    REQUIRE(cpu_result[1] > cpu_result[0]);

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
    CHECK(comparison.max_ulp_error == 0);
}

TEST_CASE("the levels stage is bit-exact on values that sit on a bucket boundary",
          "[gpu][spectrum][levels][m1]") {
    REVENANT_NEEDS_GPU();
    REVENANT_NEEDS_REPRODUCIBLE_SHARED_MEMORY();
    INFO("running on " << test::shared_context_description());

    // The bucket index is a subtract and a multiply, which is exactly the
    // shape a shader compiler fuses into a fused multiply-add. A fused one
    // rounds once where two operations round twice, so a value sitting on a
    // boundary lands in a different bucket on the two sides and the whole
    // frame's percentile moves. Uniform random input almost never puts mass
    // on a boundary; this frame puts most of its mass there.
    constexpr std::uint64_t kSeed = 0x4C564C5300000004ULL;

    std::vector<float> frame(kEngineBins, 0.0F);
    test::SeededInput input(kSeed);
    const auto jitter = input.reals(kEngineBins, -1.0F, 1.0F);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        // Exact bucket edges, spread over a few hundred buckets, with a
        // sixteenth of a bucket of jitter on a tenth of them so the
        // distribution is not degenerate.
        const auto bucket = static_cast<std::uint32_t>(300 + (i % 400));
        float value = dsp::kSpectrumLevelsRangeFloorDb +
                      static_cast<float>(bucket) * dsp::kSpectrumLevelsDbPerBucket;
        if (i % 10 == 0) {
            value += jitter[i] * dsp::kSpectrumLevelsDbPerBucket * 0.0625F;
        }
        frame[i] = value;
    }

    const auto params = make_params(kEngineBins);
    const auto gpu_result = run_levels_on_gpu(params, frame, 256);
    const auto cpu_result = run_levels_on_cpu(params, frame);

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
}

TEST_CASE("the levels stage is bit-exact at every workgroup width",
          "[gpu][spectrum][levels][m1]") {
    REVENANT_NEEDS_GPU();
    REVENANT_NEEDS_REPRODUCIBLE_SHARED_MEMORY();
    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    // The width is a scheduling decision and must not change the answer. It
    // is the property this kernel most needs asserted, because the counting
    // goes through atomics into shared memory: the invocations arrive in
    // whatever order the device chooses, and only the fact that integer
    // addition commutes makes the result independent of it. A float
    // accumulator here would fail this case and pass every other one.
    constexpr std::uint64_t kSeed = 0x4C564C5300000005ULL;

    const auto frame = synthetic_frame(kSeed, kEngineBins, -95.0F, 40.0F, 512, -6.0F);
    const auto params = make_params(kEngineBins);
    const auto cpu_result = run_levels_on_cpu(params, frame);

    for (const std::uint32_t local_size : {1U, 32U, 64U, 128U, 256U}) {
        if (local_size > context.info().max_workgroup_size_x) {
            continue;
        }
        const auto gpu_result = run_levels_on_gpu(params, frame, local_size);
        const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
        INFO("local_size_x = " << local_size);
        INFO(comparison.report);
        CHECK(comparison.identical);
    }
}

TEST_CASE("the levels stage is bit-exact across the frame sizes the engine builds",
          "[gpu][spectrum][levels][shipped][m1]") {
    REVENANT_NEEDS_GPU();
    REVENANT_NEEDS_REPRODUCIBLE_SHARED_MEMORY();
    INFO("running on " << test::shared_context_description());

    // Frame width is the only axis this kernel has, since it takes no
    // specialization constants beyond the workgroup size. These are
    // channels * transform / 2 for the grids the engine builds, plus one
    // deliberately awkward size that is not a multiple of the workgroup
    // width, where the strided loop's tail is.
    constexpr std::uint64_t kSeed = 0x4C564C5300000006ULL;

    const std::uint32_t widths[] = {
        8U * (256U / 2U),    // a small grid, 1024 bins
        64U * (256U / 2U),   // the engine's grid at a short transform
        kEngineBins,         // the engine's grid at the shipped transform
        1U,                  // one bin, where every percentile is the same bin
        4095U,               // a tail on every workgroup width tried
    };

    std::size_t failed = 0;
    std::string report = "\n";
    for (const std::uint32_t bins : widths) {
        const auto frame = synthetic_frame(kSeed, bins, -120.0F, 55.0F, bins / 512, -3.0F);
        const auto params = make_params(bins);

        const auto gpu_result = run_levels_on_gpu(params, frame, 256);
        const auto cpu_result = run_levels_on_cpu(params, frame);

        const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
        if (!comparison.identical) {
            ++failed;
        }

        // Accumulated into one string rather than reported per width. Catch2
        // clears an INFO at the next assertion and there are REQUIREs inside
        // run_levels_on_gpu, so a per-width message is gone before the
        // verdict below runs.
        report += std::format("{} bins: {} ({:.3f} to {:.3f} dB)\n", bins,
                              comparison.identical
                                  ? std::string{"exact"}
                                  : std::format("{} ulp worst", comparison.max_ulp_error),
                              cpu_result[0], cpu_result[1]);
    }

    INFO(report);
    CHECK(failed == 0);
}

TEST_CASE("the levels stage reads the frame the spectrum stage wrote",
          "[gpu][spectrum][levels][m1]") {
    REVENANT_NEEDS_GPU();
    REVENANT_NEEDS_REPRODUCIBLE_SHARED_MEMORY();
    INFO("running on " << test::shared_context_description());

    // Every case above feeds this kernel a frame made up on the host, which
    // proves the arithmetic and not the join. The engine dispatches the two
    // back to back with the spectrum kernel's output as this one's input, so
    // one case runs them that way: a real channel ring through
    // core/shaders/spectrum.comp, then core/shaders/spectrum_levels.comp over
    // what came out, against the two twins chained the same way.
    constexpr std::uint64_t kSeed = 0x4C564C5300000007ULL;
    constexpr std::uint32_t kChannels = 64;
    constexpr std::uint32_t kTransform = 256;
    constexpr std::uint32_t kChanBlocks = 512;

    struct SpectrumPush {
        std::uint32_t chan_blocks = 0;
        std::uint32_t chan_mask = 0;
        std::uint32_t in_offset = 0;
    };

    dsp::SpectrumParams spectrum_params;
    spectrum_params.channels = kChannels;
    spectrum_params.transform = kTransform;
    spectrum_params.stages = dsp::fft_stages(kTransform);
    spectrum_params.chan_blocks = kChanBlocks;
    spectrum_params.chan_mask = kChanBlocks - 1U;
    spectrum_params.in_offset = kChanBlocks - 37U;

    auto twiddles = dsp::build_twiddles(kTransform);
    INFO(test::message_of(twiddles));
    REQUIRE(twiddles.has_value());
    auto window = dsp::build_spectrum_window(kTransform);
    INFO(test::message_of(window));
    REQUIRE(window.has_value());

    test::SeededInput input(kSeed);
    const auto channel_ring =
        input.complexes(static_cast<std::size_t>(kChannels) * kChanBlocks);

    const auto bins = dsp::spectrum_bin_count(kChannels, kTransform);
    std::vector<float> gpu_frame(bins, 0.0F);
    {
        const std::uint32_t constants[] = {spectrum_params.channels, spectrum_params.transform,
                                           spectrum_params.stages};
        const SpectrumPush push{
            .chan_blocks = spectrum_params.chan_blocks,
            .chan_mask = spectrum_params.chan_mask,
            .in_offset = spectrum_params.in_offset,
        };

        gpu::KernelInvocation invocation;
        invocation.spirv = gpu::shaders::spectrum();
        invocation.inputs = {std::as_bytes(std::span<const dsp::Complex32>(channel_ring)),
                             std::as_bytes(std::span<const dsp::Complex32>(*twiddles)),
                             std::as_bytes(std::span<const float>(*window))};
        invocation.outputs = {std::as_writable_bytes(std::span<float>(gpu_frame))};
        invocation.push_constants = std::as_bytes(std::span<const SpectrumPush>(&push, 1));
        invocation.local_size_x = 128;
        invocation.group_count_x = kChannels;
        invocation.invocations = kChannels * 128U;
        invocation.grid_constants.assign(std::begin(constants), std::end(constants));

        const auto ran = gpu::run_kernel(test::shared_context(), invocation);
        INFO(test::message_of(ran));
        REQUIRE(ran.has_value());
    }

    std::vector<float> cpu_frame(bins, 0.0F);
    {
        const auto ran = dsp::reference_spectrum(spectrum_params, channel_ring, *twiddles,
                                                 *window, cpu_frame);
        INFO(test::message_of(ran));
        REQUIRE(ran.has_value());
    }

    const auto params = make_params(bins);
    const auto gpu_result = run_levels_on_gpu(params, gpu_frame, 256);
    const auto cpu_result = run_levels_on_cpu(params, cpu_frame);

    INFO(std::format("chained: gpu {} to {}, cpu {} to {}", gpu_result[0], gpu_result[1],
                     cpu_result[0], cpu_result[1]));
    REQUIRE(cpu_result[1] > cpu_result[0]);

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
}
