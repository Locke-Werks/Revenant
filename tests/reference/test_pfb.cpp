// The polyphase channelizer, GPU against CPU, bit for bit.
//
// This is the case the whole architecture rests on. Channels being nearly free
// is what makes two hundred receivers cost what two cost, and every feature
// above Layer 1 is downstream of it. If these two kernels are wrong, they are
// wrong in a way that produces a waterfall which looks entirely plausible.
//
// Three kinds of case here, and they catch different things:
//
//   The host design tests check the filter and twiddle tables against their
//   own stated properties. They need no GPU.
//
//   The bit-exact tests run each kernel against its twin and demand identical
//   bits. They are what the conformance matrix runs on every device.
//
//   The end-to-end tests put a tone at a known frequency through both stages
//   and check it lands in the channel it should. A bit-exact kernel that
//   agrees with a twin implementing the wrong convention is still wrong, and
//   only this class of test notices.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/pfb_branch_reference.h"
#include "core/dsp/pfb_fft_reference.h"
#include "core/dsp/types.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;
using Catch::Approx;

namespace {

// The canonical grid: 64 channels, 17 taps per branch including the delay pad,
// 2x oversampled. Small enough to run quickly and large enough that the branch
// reversal, the twiddle table and the shared-memory transform all do something
// non-trivial.
constexpr std::uint32_t kChannels = 64;
constexpr std::uint32_t kTapsPerBranch = 17;
constexpr std::uint32_t kDecimation = 32;
constexpr std::uint32_t kStages = 6;  // log2(64)

constexpr dsp::GridParams kGrid{
    .channels = kChannels,
    .taps_per_branch = kTapsPerBranch,
    .decimation = kDecimation,
};

// Input ring. A power of two, as the kernel's mask arithmetic requires.
constexpr std::uint32_t kRingSamples = 1U << 14;
constexpr std::uint32_t kRingMask = kRingSamples - 1U;

// Deliberately close to the top of the ring, so every case wraps. The
// (base - n) & mask arithmetic is the single most suspicious-looking line in
// the branch kernel and the one most worth exercising on a real driver.
constexpr std::uint32_t kBaseOffset = kRingSamples - 40U;

constexpr std::uint32_t kBlocks = 8;
constexpr std::uint32_t kOutRingBlocks = 16;  // power of two, at least kBlocks
constexpr std::uint32_t kOutRingMask = kOutRingBlocks - 1U;

// Mirrors the push constant block of core/shaders/pfb_fft.comp.
struct FftPush {
    std::uint32_t block_base = 0;
    std::uint32_t block_count = 0;
    std::uint32_t out_ring_blocks = 0;
    std::uint32_t out_ring_mask = 0;
};

std::vector<dsp::Complex32> run_branch_on_gpu(std::span<const dsp::Complex32> ring,
                                              std::span<const float> taps,
                                              const dsp::PfbBranchParams& params,
                                              std::uint32_t local_size) {
    auto& context = test::shared_context();

    std::vector<dsp::Complex32> out(static_cast<std::size_t>(params.block_count) * kChannels,
                                    dsp::Complex32{});

    const std::uint32_t grid_constants[] = {kChannels, kTapsPerBranch, kDecimation};

    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::pfb_branch();
    invocation.inputs = {std::as_bytes(ring), std::as_bytes(taps)};
    invocation.outputs = {std::as_writable_bytes(std::span<dsp::Complex32>(out))};
    invocation.push_constants = std::as_bytes(std::span<const dsp::PfbBranchParams>(&params, 1));
    invocation.invocations = params.block_count * kChannels;
    invocation.local_size_x = local_size;
    invocation.grid_constants.assign(std::begin(grid_constants), std::end(grid_constants));

    const auto ran = gpu::run_kernel(context, invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
    return out;
}

std::vector<dsp::Complex32> run_fft_on_gpu(std::span<const dsp::Complex32> branch_values,
                                           std::span<const dsp::Complex32> twiddles,
                                           const FftPush& push,
                                           std::uint32_t local_size) {
    auto& context = test::shared_context();

    std::vector<dsp::Complex32> out(
        static_cast<std::size_t>(kChannels) * push.out_ring_blocks, dsp::Complex32{});

    const std::uint32_t grid_constants[] = {kChannels, kDecimation, kStages};

    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::pfb_fft();
    invocation.inputs = {std::as_bytes(branch_values), std::as_bytes(twiddles)};
    invocation.outputs = {std::as_writable_bytes(std::span<dsp::Complex32>(out))};
    invocation.push_constants = std::as_bytes(std::span<const FftPush>(&push, 1));
    invocation.local_size_x = local_size;
    // One workgroup per transform. The FFT holds a whole block in shared
    // memory and indexes by workgroup, so the group count is the block count
    // and has nothing to do with the thread count within a block.
    invocation.group_count_x = push.block_count;
    invocation.invocations = push.block_count * local_size;
    invocation.grid_constants.assign(std::begin(grid_constants), std::end(grid_constants));

    const auto ran = gpu::run_kernel(context, invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
    return out;
}

dsp::PrototypeFilter make_prototype() {
    auto designed = dsp::design_prototype(kGrid, 120.0);
    INFO(test::message_of(designed));
    REQUIRE(designed.has_value());
    return *designed;
}

std::vector<dsp::Complex32> make_twiddles() {
    auto built = dsp::build_twiddles(kChannels);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());
    return *built;
}

}  // namespace

// ---------------------------------------------------------------------------
// Host design, no GPU needed
// ---------------------------------------------------------------------------

TEST_CASE("the prototype filter meets the properties the design claims", "[pfb][m1]") {
    const auto prototype = make_prototype();

    CHECK(prototype.taps.size() == kGrid.prototype_length());

    // The reason the prototype is padded to M*L+1 rather than being M*L: it
    // makes the group delay an integer number of channel samples, which is
    // what keeps a sample index exact through channelization. Without it the
    // delay is a rational that is neither integer nor half-integer, and the
    // sample-accurate timestamp guarantee quietly stops being true.
    const auto taps_l = kTapsPerBranch - 1;
    CHECK(prototype.group_delay_samples == static_cast<dsp::SampleIndex>(kChannels) * taps_l / 2);
    CHECK(prototype.group_delay_samples % kDecimation == 0);

    // Designed to 120 dB. Measured, not estimated, by the designer.
    INFO("measured stopband " << prototype.stopband_db << " dB");
    CHECK(prototype.stopband_db < -110.0);

    // The pad entries are exact zeros, so they cost arithmetic and not
    // accuracy.
    const std::size_t designed_taps = static_cast<std::size_t>(kChannels) * taps_l + 1;
    for (std::size_t i = designed_taps; i < prototype.taps.size(); ++i) {
        INFO("pad index " << i);
        CHECK(prototype.taps[i] == 0.0F);
    }

    // Normalised so a tone at a channel centre reads unit magnitude.
    double sum = 0.0;
    for (const float tap : prototype.taps) {
        sum += static_cast<double>(tap);
    }
    CHECK(sum == Approx(1.0).margin(1e-6));
}

TEST_CASE("the twiddle table has exact quadrature entries", "[pfb][m1]") {
    const auto twiddles = make_twiddles();
    REQUIRE(twiddles.size() == kChannels);

    // These four have to be bit-exact, not merely close. A residue in the
    // imaginary part of W^(M/2) turns what should be a free sign flip into a
    // real complex multiply, and costs bit-exactness for nothing. Computing
    // the table naively from cos(pi) leaves exactly that residue, which is why
    // it is built from the first octant and reflected.
    CHECK(twiddles[0] == dsp::Complex32{1.0F, 0.0F});
    CHECK(twiddles[kChannels / 4] == dsp::Complex32{0.0F, -1.0F});
    CHECK(twiddles[kChannels / 2] == dsp::Complex32{-1.0F, 0.0F});
    CHECK(twiddles[3 * kChannels / 4] == dsp::Complex32{0.0F, 1.0F});

    // No negative zeros anywhere: -0.0 and +0.0 compare equal but are
    // different bit patterns and propagate differently through a division.
    auto is_negative_zero = [](float value) {
        return value == 0.0F && std::signbit(value);
    };
    for (std::size_t j = 0; j < twiddles.size(); ++j) {
        INFO("twiddle " << j);
        CHECK_FALSE(is_negative_zero(twiddles[j].real()));
        CHECK_FALSE(is_negative_zero(twiddles[j].imag()));
    }

    // Every entry sits on the unit circle to within float rounding.
    double worst = 0.0;
    for (const auto& w : twiddles) {
        worst = std::max(worst, std::abs(std::abs(std::complex<double>(w)) - 1.0));
    }
    INFO("worst |W| deviation " << worst);
    CHECK(worst < 1e-7);
}

// ---------------------------------------------------------------------------
// Bit-exact, the conformance cases
// ---------------------------------------------------------------------------

TEST_CASE("the branch filter matches its CPU twin bit-exactly", "[gpu][pfb][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x5046420000000001ULL;
    const auto prototype = make_prototype();

    test::SeededInput input(kSeed);
    const auto ring = input.complexes(kRingSamples);

    const dsp::PfbBranchParams params{
        .ring_mask = kRingMask,
        .base_offset = kBaseOffset,
        .block_count = kBlocks,
    };

    const auto gpu_result = run_branch_on_gpu(ring, prototype.taps, params, 64);

    std::vector<dsp::Complex32> cpu_result(gpu_result.size(), dsp::Complex32{});
    const auto computed =
        dsp::reference_pfb_branches(kGrid, prototype.taps, ring, params, cpu_result);
    INFO(test::message_of(computed));
    REQUIRE(computed.has_value());

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
    CHECK(comparison.max_ulp_error == 0);
}

TEST_CASE("the branch filter is bit-exact on values chosen to provoke rounding",
          "[gpu][pfb][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The accumulate-multiply in the tap loop is exactly the shape a compiler
    // fuses into an FMA. Uniform input in [-1, 1] rarely distinguishes a fused
    // multiply-add from two separate operations; these values do.
    constexpr std::uint64_t kSeed = 0x5046420000000002ULL;
    const auto prototype = make_prototype();

    test::SeededInput input(kSeed);
    const auto ring = input.adversarial_complexes(kRingSamples);

    const dsp::PfbBranchParams params{
        .ring_mask = kRingMask,
        .base_offset = kBaseOffset,
        .block_count = kBlocks,
    };

    const auto gpu_result = run_branch_on_gpu(ring, prototype.taps, params, 64);

    std::vector<dsp::Complex32> cpu_result(gpu_result.size(), dsp::Complex32{});
    REQUIRE(dsp::reference_pfb_branches(kGrid, prototype.taps, ring, params, cpu_result)
                .has_value());

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
}

TEST_CASE("the branch filter is bit-exact at every workgroup size", "[gpu][pfb][m1]") {
    REVENANT_NEEDS_GPU();
    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x5046420000000003ULL;
    const auto prototype = make_prototype();

    test::SeededInput input(kSeed);
    const auto ring = input.complexes(kRingSamples);

    const dsp::PfbBranchParams params{
        .ring_mask = kRingMask,
        .base_offset = kBaseOffset,
        .block_count = kBlocks,
    };

    std::vector<dsp::Complex32> cpu_result(
        static_cast<std::size_t>(kBlocks) * kChannels, dsp::Complex32{});
    REQUIRE(dsp::reference_pfb_branches(kGrid, prototype.taps, ring, params, cpu_result)
                .has_value());

    // The workgroup size is a scheduling decision and must not change the
    // answer. A kernel whose output depends on it has a race or an
    // out-of-bounds read that happens to be benign at one size.
    for (const std::uint32_t local_size : {32U, 64U, 128U, 256U}) {
        if (local_size > context.info().max_workgroup_size_x) {
            continue;
        }
        const auto gpu_result = run_branch_on_gpu(ring, prototype.taps, params, local_size);
        const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
        INFO("local_size_x = " << local_size);
        INFO(comparison.report);
        CHECK(comparison.identical);
    }
}

TEST_CASE("the FFT stage matches its CPU twin bit-exactly", "[gpu][pfb][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x5046420000000004ULL;
    const auto twiddles = make_twiddles();

    // Feed the transform the branch stage's own output shape, so the test
    // exercises the layout the two kernels actually exchange.
    test::SeededInput input(kSeed);
    const auto branch_values = input.complexes(static_cast<std::size_t>(kBlocks) * kChannels);

    const FftPush push{
        .block_base = 0,
        .block_count = kBlocks,
        .out_ring_blocks = kOutRingBlocks,
        .out_ring_mask = kOutRingMask,
    };

    const auto gpu_result = run_fft_on_gpu(branch_values, twiddles, push, 64);

    const dsp::PfbFftParams params{
        .channels = kChannels,
        .decimation = kDecimation,
        .stages = kStages,
        .block_base = push.block_base,
        .block_count = push.block_count,
        .out_ring_blocks = push.out_ring_blocks,
        .out_ring_mask = push.out_ring_mask,
    };
    REQUIRE(dsp::validate(params).has_value());

    std::vector<dsp::Complex32> cpu_result(gpu_result.size(), dsp::Complex32{});
    const auto computed = dsp::reference_pfb_fft(params, branch_values, twiddles, cpu_result);
    INFO(test::message_of(computed));
    REQUIRE(computed.has_value());

    std::size_t cpu_nonzero = 0;
    std::size_t gpu_nonzero = 0;
    for (std::size_t i = 0; i < cpu_result.size(); ++i) {
        if (cpu_result[i] != dsp::Complex32{}) {
            ++cpu_nonzero;
        }
        if (gpu_result[i] != dsp::Complex32{}) {
            ++gpu_nonzero;
        }
    }
    INFO("cpu nonzero " << cpu_nonzero << " of " << cpu_result.size() << ", gpu nonzero "
                        << gpu_nonzero);

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
    CHECK(comparison.max_ulp_error == 0);
}

TEST_CASE("the FFT stage is bit-exact at every workgroup size", "[gpu][pfb][m1]") {
    REVENANT_NEEDS_GPU();
    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x5046420000000005ULL;
    const auto twiddles = make_twiddles();

    test::SeededInput input(kSeed);
    const auto branch_values = input.complexes(static_cast<std::size_t>(kBlocks) * kChannels);

    const FftPush push{
        .block_base = 3,  // odd, so the D == M/2 phase correction is exercised
        .block_count = kBlocks,
        .out_ring_blocks = kOutRingBlocks,
        .out_ring_mask = kOutRingMask,
    };

    const dsp::PfbFftParams params{
        .channels = kChannels,
        .decimation = kDecimation,
        .stages = kStages,
        .block_base = push.block_base,
        .block_count = push.block_count,
        .out_ring_blocks = push.out_ring_blocks,
        .out_ring_mask = push.out_ring_mask,
    };

    std::vector<dsp::Complex32> cpu_result(
        static_cast<std::size_t>(kChannels) * kOutRingBlocks, dsp::Complex32{});
    REQUIRE(dsp::reference_pfb_fft(params, branch_values, twiddles, cpu_result).has_value());

    for (const std::uint32_t local_size : {16U, 32U, 64U, 128U}) {
        if (local_size > context.info().max_workgroup_size_x) {
            continue;
        }
        const auto gpu_result = run_fft_on_gpu(branch_values, twiddles, push, local_size);
        const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
        INFO("local_size_x = " << local_size);
        INFO(comparison.report);
        CHECK(comparison.identical);
    }
}

// ---------------------------------------------------------------------------
// End to end, which is what catches a convention that is wrong consistently
// ---------------------------------------------------------------------------

TEST_CASE("a tone lands in the channel it belongs to", "[gpu][pfb][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The test the branch reversal exists for.
    //
    // Both kernels can be bit-exact against twins that share a wrong
    // convention, and the result is a spectrum mirrored about DC: channel k
    // carrying what channel M-k should. A waterfall of that looks completely
    // normal. Nothing but an end-to-end check against a known input notices,
    // which is why this case is here and why it tests several channels rather
    // than one.
    const auto prototype = make_prototype();
    const auto twiddles = make_twiddles();

    for (const std::uint32_t tone_channel : {1U, 3U, 7U, 16U, 61U, 63U}) {
        // A tone exactly at the centre of channel k: exp(+j*2*pi*k*n/M).
        std::vector<dsp::Complex32> ring(kRingSamples);
        for (std::uint32_t n = 0; n < kRingSamples; ++n) {
            const double phase =
                2.0 * 3.14159265358979323846 * static_cast<double>(tone_channel) *
                static_cast<double>(n) / static_cast<double>(kChannels);
            ring[n] = dsp::Complex32{static_cast<float>(std::cos(phase)),
                                     static_cast<float>(std::sin(phase))};
        }

        // Start far enough in that the filter history is entirely tone.
        const dsp::PfbBranchParams branch_params{
            .ring_mask = kRingMask,
            .base_offset = 4096,
            .block_count = kBlocks,
        };

        const auto branch_values =
            run_branch_on_gpu(ring, prototype.taps, branch_params, 64);

        const FftPush push{
            .block_base = 0,
            .block_count = kBlocks,
            .out_ring_blocks = kOutRingBlocks,
            .out_ring_mask = kOutRingMask,
        };
        const auto channels = run_fft_on_gpu(branch_values, twiddles, push, 64);

        // Read the last block, by which point the filter is fully settled.
        const std::uint32_t block = kBlocks - 1;
        std::uint32_t peak = 0;
        double peak_mag = -1.0;
        double total_other = 0.0;
        for (std::uint32_t k = 0; k < kChannels; ++k) {
            const auto value = channels[static_cast<std::size_t>(k) * kOutRingBlocks +
                                        (block & kOutRingMask)];
            const double mag = std::abs(std::complex<double>(value));
            if (mag > peak_mag) {
                peak_mag = mag;
                peak = k;
            }
        }
        for (std::uint32_t k = 0; k < kChannels; ++k) {
            if (k == peak) {
                continue;
            }
            const auto value = channels[static_cast<std::size_t>(k) * kOutRingBlocks +
                                        (block & kOutRingMask)];
            total_other = std::max(total_other, std::abs(std::complex<double>(value)));
        }

        INFO("tone at channel " << tone_channel << " peaked in channel " << peak
                                << " magnitude " << peak_mag << ", worst other channel "
                                << total_other);

        // The tone must appear in its own channel, not in channel M-k. This is
        // the assertion that fails if the branch reversal is dropped.
        CHECK(peak == tone_channel);

        // Unit magnitude, because the prototype is normalised so a tone at a
        // channel centre reads 1.0.
        CHECK(peak_mag == Approx(1.0).margin(0.02));

        // And the other channels are far down. 60 dB is a loose bound chosen
        // so the case reports a convention error rather than a filter-design
        // regression; the prototype's own stopband is asserted separately.
        CHECK(total_other < peak_mag * 1e-3);
    }
}
