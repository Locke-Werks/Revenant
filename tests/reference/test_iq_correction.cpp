// The front-end correction's two kernels, GPU against CPU, bit for bit.
//
// core/shaders/iq_moments.comp sums five moments over fixed chunks of a block
// and core/shaders/iq_correct.comp applies the DC and I/Q correction in place.
// Both are single correctly rounded operations under `precise`, so their twins
// in core/dsp/front_end_correction_reference.cpp are held to identical bits,
// at several workgroup widths, through a ring wrap, on a block that is not a
// whole number of chunks, and on inputs built to show a fused multiply-add.
//
// The moments kernel's summation order is fixed by the block and not by the
// workgroup, which is the property that lets one twin referee every width.
// The width cases below are what check that claim rather than trust it.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/front_end_correction.h"
#include "core/dsp/types.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

constexpr std::uint32_t kRingSamples = 1U << 14;
constexpr std::uint32_t kRingMask = kRingSamples - 1U;
constexpr std::uint64_t kSeed = 20260923;

// Starts 1000 samples before the end of the ring and runs 5000 past it, so
// the block wraps, and 6000 is not a multiple of the 64-sample chunk, so the
// last chunk is short.
constexpr std::uint32_t kOffset = kRingSamples - 1000U;
constexpr std::uint32_t kCount = 6000U;

std::vector<float> moments_on_gpu(std::span<const dsp::Complex32> ring,
                                  const dsp::IqMomentsParams& params, std::uint32_t local_size) {
    std::vector<float> sums(static_cast<std::size_t>(dsp::iq_moments_chunks(params.count)) *
                                dsp::kIqMomentsPerChunk,
                            0.0F);
    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::iq_moments();
    invocation.inputs = {std::as_bytes(ring)};
    invocation.outputs = {std::as_writable_bytes(std::span<float>(sums))};
    invocation.push_constants = std::as_bytes(std::span<const dsp::IqMomentsParams>(&params, 1));
    invocation.invocations = dsp::iq_moments_chunks(params.count);
    invocation.local_size_x = local_size;
    const auto ran = gpu::run_kernel(test::shared_context(), invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
    return sums;
}

std::vector<dsp::Complex32> correct_on_gpu(std::span<const dsp::Complex32> ring,
                                           const dsp::IqCorrectParams& params,
                                           std::uint32_t local_size) {
    std::vector<dsp::Complex32> out(ring.size(), dsp::Complex32{});
    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::iq_correct();
    invocation.inputs = {std::as_bytes(ring)};
    invocation.outputs = {std::as_writable_bytes(std::span<dsp::Complex32>(out))};
    invocation.push_constants = std::as_bytes(std::span<const dsp::IqCorrectParams>(&params, 1));
    invocation.invocations = params.count;
    invocation.local_size_x = local_size;
    const auto ran = gpu::run_kernel(test::shared_context(), invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
    return out;
}

// A signal-shaped ring: a tone with an offset and an imbalance, then noise.
std::vector<dsp::Complex32> signal_ring() {
    test::SeededInput input(kSeed);
    std::vector<dsp::Complex32> ring = input.complexes(kRingSamples, -0.3F, 0.3F);
    for (std::uint32_t n = 0; n < kRingSamples; ++n) {
        const float angle = 0.013F * static_cast<float>(n);
        ring[n] += dsp::Complex32{0.02F + 0.1F * std::cos(angle), -0.01F + 0.105F * std::sin(angle)};
    }
    return ring;
}

}  // namespace

TEST_CASE("the moments kernel matches its twin bit for bit at every workgroup width",
          "[gpu][reference][calibration]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description() << ", seed " << kSeed);

    for (const bool adversarial : {false, true}) {
        test::SeededInput input(kSeed + 1);
        const std::vector<dsp::Complex32> ring =
            adversarial ? input.adversarial_complexes(kRingSamples) : signal_ring();

        const dsp::IqMomentsParams params{
            .ring_mask = kRingMask, .src_offset = kOffset, .count = kCount};
        std::vector<float> cpu(static_cast<std::size_t>(dsp::iq_moments_chunks(kCount)) *
                                   dsp::kIqMomentsPerChunk,
                               0.0F);
        REQUIRE(dsp::reference_iq_moments(ring, params, cpu).has_value());

        for (const std::uint32_t local_size : {1U, 32U, 64U, 256U}) {
            if (local_size > test::shared_context().info().max_workgroup_size_x) {
                continue;
            }
            const std::vector<float> gpu = moments_on_gpu(ring, params, local_size);
            const auto comparison = test::diff(gpu, cpu, kSeed);
            INFO((adversarial ? "adversarial" : "signal") << " input, local_size_x = "
                                                          << local_size << "\n"
                                                          << comparison.report);
            CHECK(comparison.identical);
        }
    }
}

TEST_CASE("the correction kernel matches its twin bit for bit, through a wrap",
          "[gpu][reference][calibration]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description() << ", seed " << kSeed);

    // Coefficients of the size a real estimate produces, and the identity.
    const dsp::IqCorrectParams real{.ring_mask = kRingMask,
                                    .offset = kOffset,
                                    .count = kCount,
                                    .dc_i = 0.0213F,
                                    .dc_q = -0.0097F,
                                    .cross = -0.0524078F,
                                    .scale = 0.9536746F};
    dsp::IqCorrectParams identity = real;
    identity.dc_i = 0.0F;
    identity.dc_q = 0.0F;
    identity.cross = 0.0F;
    identity.scale = 1.0F;

    for (const bool adversarial : {false, true}) {
        test::SeededInput input(kSeed + 2);
        const std::vector<dsp::Complex32> ring =
            adversarial ? input.adversarial_complexes(kRingSamples) : signal_ring();

        for (const dsp::IqCorrectParams& params : {real, identity}) {
            std::vector<dsp::Complex32> cpu(kRingSamples, dsp::Complex32{});
            REQUIRE(dsp::reference_iq_correct(ring, params, cpu).has_value());

            for (const std::uint32_t local_size : {1U, 64U, 256U}) {
                if (local_size > test::shared_context().info().max_workgroup_size_x) {
                    continue;
                }
                const std::vector<dsp::Complex32> gpu = correct_on_gpu(ring, params, local_size);
                const auto comparison = test::diff(gpu, cpu, kSeed);
                INFO((adversarial ? "adversarial" : "signal")
                     << " input, cross " << params.cross << ", local_size_x = " << local_size
                     << "\n"
                     << comparison.report);
                CHECK(comparison.identical);
            }
        }
    }
}

TEST_CASE("the correction twin corrects what the model impairs", "[reference][calibration]") {
    // Not a GPU case: the twin's arithmetic against the model in
    // core/dsp/front_end_correction.h, so a wrong sign in either is caught
    // before any device agrees with it. A tone through I_r = I,
    // Q_r = g (Q cos(phi) + I sin(phi)) comes back as the tone.
    constexpr float kGain = 1.05F;
    constexpr double kPhi = 3.0 * 3.14159265358979323846 / 180.0;
    std::vector<dsp::Complex32> ring(256);
    std::vector<dsp::Complex32> ideal(256);
    for (std::uint32_t n = 0; n < 256; ++n) {
        const double angle = 0.1 * static_cast<double>(n);
        const double i = std::cos(angle);
        const double q = std::sin(angle);
        ideal[n] = dsp::Complex32{static_cast<float>(i), static_cast<float>(q)};
        ring[n] = dsp::Complex32{static_cast<float>(i),
                                 static_cast<float>(kGain * (q * std::cos(kPhi) + i * std::sin(kPhi)))};
    }
    const dsp::IqCorrectParams params{
        .ring_mask = 255,
        .offset = 0,
        .count = 256,
        .dc_i = 0.0F,
        .dc_q = 0.0F,
        .cross = static_cast<float>(-std::tan(kPhi)),
        .scale = static_cast<float>(1.0 / (static_cast<double>(kGain) * std::cos(kPhi)))};
    std::vector<dsp::Complex32> out(256);
    REQUIRE(dsp::reference_iq_correct(ring, params, out).has_value());
    float worst = 0.0F;
    for (std::uint32_t n = 0; n < 256; ++n) {
        worst = std::max(worst, std::abs(out[n] - ideal[n]));
    }
    INFO("worst error " << worst);
    CHECK(worst < 1.0e-6F);
}
