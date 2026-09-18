// The format-conversion kernels, GPU against CPU, bit for bit.
//
// These exist so that a radio's native bytes cross the bus exactly once. An
// RTL-SDR delivers unsigned 8-bit pairs; converting on the host would put a
// pass over every sample back on the CPU and quadruple what crosses the bus,
// which at 20 MS/s is 40 MB/s against 160. That is the cost the whole
// architecture exists to avoid, so the widening happens on the device as part
// of the upload.
//
// Two classes of case. The exhaustive host cases walk every code an 8-bit
// format can produce and check the value and its bit pattern, because the
// conversion is a table small enough to verify completely and there is no
// excuse for sampling it. The GPU cases demand bit-identical output from the
// kernels, including through a ring wrap.

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/convert.h"
#include "core/dsp/types.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// A ring small enough to force wraps and a power of two, as the mask
// arithmetic requires.
constexpr std::uint32_t kRingSamples = 1U << 12;
constexpr std::uint32_t kRingMask = kRingSamples - 1U;

std::vector<dsp::Complex32> run_convert_on_gpu(std::span<const std::uint32_t> spirv,
                                               std::span<const std::byte> packed,
                                               const dsp::ConvertParams& params,
                                               std::uint32_t local_size) {
    auto& context = test::shared_context();

    std::vector<dsp::Complex32> ring(kRingSamples, dsp::Complex32{});

    gpu::KernelInvocation invocation;
    invocation.spirv = spirv;
    invocation.inputs = {packed};
    invocation.outputs = {std::as_writable_bytes(std::span<dsp::Complex32>(ring))};
    invocation.push_constants = std::as_bytes(std::span<const dsp::ConvertParams>(&params, 1));
    invocation.invocations = params.count;
    invocation.local_size_x = local_size;

    const auto ran = gpu::run_kernel(context, invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
    return ring;
}

std::uint32_t bits_of(float value) { return std::bit_cast<std::uint32_t>(value); }

}  // namespace

// ---------------------------------------------------------------------------
// Exhaustive host cases. No GPU needed, and no sampling.
// ---------------------------------------------------------------------------

TEST_CASE("every cu8 code converts to the value it should", "[convert][m1]") {
    // cu8 is OFFSET BINARY: 128 is zero, not 0. The scale is (v - 127.5)/127.5
    // and the 127.5 is the part worth understanding. Using 128 would map code
    // 0 to -1 and code 255 to +0.9922, an asymmetric range whose mean is half
    // a least significant bit away from zero. In a receiver that is a DC term,
    // and a DC term in the middle of the band is a spur sitting exactly where
    // the operator is most likely to be tuned. Centring on 127.5 removes it by
    // construction rather than by a later correction stage.
    CHECK(dsp::cu8_to_float(0) == -1.0F);
    CHECK(dsp::cu8_to_float(255) == 1.0F);

    // Antisymmetric, bit for bit, which is the zero-DC property stated exactly
    // rather than approximately. Checked over the whole table because the
    // table has only 256 entries.
    for (int code = 0; code <= 255; ++code) {
        const auto low = static_cast<std::uint8_t>(code);
        const auto high = static_cast<std::uint8_t>(255 - code);
        const float a = dsp::cu8_to_float(low);
        const float b = dsp::cu8_to_float(high);
        INFO("code " << code);
        CHECK(bits_of(a) == (bits_of(b) ^ 0x80000000U));
    }

    // No code is zero, of either sign, because 127.5 is not a code. A negative
    // zero here would be a different bit pattern that compares equal, and it
    // would propagate differently through a later division.
    for (int code = 0; code <= 255; ++code) {
        const float value = dsp::cu8_to_float(static_cast<std::uint8_t>(code));
        INFO("code " << code);
        CHECK(value != 0.0F);
    }

    // Monotone. A conversion table that is not strictly increasing has an
    // arithmetic error that no spot check would find.
    for (int code = 1; code <= 255; ++code) {
        CHECK(dsp::cu8_to_float(static_cast<std::uint8_t>(code)) >
              dsp::cu8_to_float(static_cast<std::uint8_t>(code - 1)));
    }
}

TEST_CASE("every cs8 code converts to the value it should", "[convert][m1]") {
    // Two's complement, and the asymmetry is the case people get wrong: the
    // range is -128 to +127, so no scale makes both ends map to exactly one.
    // Dividing by 128 is the choice here, which makes the scale a power of two
    // and therefore exact, at the cost of +127 mapping slightly below +1.
    CHECK(dsp::cs8_to_float(0) == 0.0F);
    CHECK(std::signbit(dsp::cs8_to_float(0)) == false);
    CHECK(dsp::cs8_to_float(-128) == -1.0F);

    for (int code = -128; code <= 127; ++code) {
        const float value = dsp::cs8_to_float(static_cast<std::int8_t>(code));
        INFO("code " << code);
        CHECK(value >= -1.0F);
        CHECK(value <= 1.0F);
    }

    for (int code = -127; code <= 127; ++code) {
        CHECK(dsp::cs8_to_float(static_cast<std::int8_t>(code)) >
              dsp::cs8_to_float(static_cast<std::int8_t>(code - 1)));
    }

    // The scale being a power of two is what makes this exact: every code
    // converts with no rounding at all, so the twin and the kernel cannot
    // disagree even in principle.
    for (int code = -128; code <= 127; ++code) {
        const float value = dsp::cs8_to_float(static_cast<std::int8_t>(code));
        INFO("code " << code);
        CHECK(value * 128.0F == static_cast<float>(code));
    }
}

TEST_CASE("cs16 conversion is exact across its range", "[convert][m1]") {
    CHECK(dsp::cs16_to_float(0) == 0.0F);
    CHECK(dsp::cs16_to_float(-32768) == -1.0F);

    // Same power-of-two argument as cs8, and the same exactness. Sampled
    // rather than exhaustive only because 65536 checks would dominate the
    // suite's runtime for no additional information: the property is
    // structural.
    for (int code = -32768; code <= 32767; code += 97) {
        const float value = dsp::cs16_to_float(static_cast<std::int16_t>(code));
        INFO("code " << code);
        CHECK(value * 32768.0F == static_cast<float>(code));
        CHECK(value >= -1.0F);
        CHECK(value <= 1.0F);
    }
}

// ---------------------------------------------------------------------------
// Bit-exact, the conformance cases
// ---------------------------------------------------------------------------

TEST_CASE("the cu8 kernel matches its CPU twin bit-exactly", "[gpu][convert][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x434F4E5600000001ULL;
    constexpr std::uint32_t kCount = 3000;

    // Every code appears, and the tail is random, so the case covers the whole
    // table and an arbitrary pattern in one dispatch.
    std::vector<std::byte> packed(static_cast<std::size_t>(kCount) * 2);
    test::SeededInput input(kSeed);
    const auto noise = input.reals(packed.size());
    for (std::size_t i = 0; i < packed.size(); ++i) {
        const auto code = i < 512 ? static_cast<std::uint8_t>(i & 0xFFU)
                                  : static_cast<std::uint8_t>(
                                        static_cast<int>((noise[i] + 1.0F) * 127.5F) & 0xFF);
        packed[i] = static_cast<std::byte>(code);
    }

    // A destination offset chosen so the written window runs off the end of
    // the ring and wraps, which is the arithmetic most worth exercising on a
    // real driver.
    const dsp::ConvertParams params{
        .ring_mask = kRingMask,
        .dst_offset = kRingSamples - 500U,
        .src_offset = 0,
        .count = kCount,
    };

    const auto gpu_result = run_convert_on_gpu(gpu::shaders::convert_cu8_cf32(), packed, params, 64);

    std::vector<dsp::Complex32> cpu_result(kRingSamples, dsp::Complex32{});
    const auto computed = dsp::reference_convert_cu8_cf32(packed, params, cpu_result);
    INFO(test::message_of(computed));
    REQUIRE(computed.has_value());

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
    CHECK(comparison.max_ulp_error == 0);
}

TEST_CASE("the cs8 and cs16 kernels match their CPU twins bit-exactly",
          "[gpu][convert][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x434F4E5600000002ULL;
    constexpr std::uint32_t kCount = 2048;

    test::SeededInput input(kSeed);

    SECTION("cs8") {
        std::vector<std::byte> packed(static_cast<std::size_t>(kCount) * 2);
        const auto noise = input.reals(packed.size());
        for (std::size_t i = 0; i < packed.size(); ++i) {
            packed[i] = static_cast<std::byte>(static_cast<std::uint8_t>(
                static_cast<int>(noise[i] * 127.0F) & 0xFF));
        }

        const dsp::ConvertParams params{
            .ring_mask = kRingMask,
            .dst_offset = kRingSamples - 777U,
            .src_offset = 0,
            .count = kCount,
        };

        const auto gpu_result =
            run_convert_on_gpu(gpu::shaders::convert_cs8_cf32(), packed, params, 64);

        std::vector<dsp::Complex32> cpu_result(kRingSamples, dsp::Complex32{});
        REQUIRE(dsp::reference_convert_cs8_cf32(packed, params, cpu_result).has_value());

        const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
        INFO(comparison.report);
        CHECK(comparison.identical);
    }

    SECTION("cs16") {
        std::vector<std::byte> packed(static_cast<std::size_t>(kCount) * 4);
        const auto noise = input.reals(packed.size() / 2);
        for (std::size_t s = 0; s < noise.size(); ++s) {
            const auto code = static_cast<std::int16_t>(noise[s] * 32000.0F);
            const auto raw = static_cast<std::uint16_t>(code);
            packed[s * 2] = static_cast<std::byte>(raw & 0xFFU);
            packed[s * 2 + 1] = static_cast<std::byte>((raw >> 8) & 0xFFU);
        }

        const dsp::ConvertParams params{
            .ring_mask = kRingMask,
            .dst_offset = kRingSamples - 100U,
            .src_offset = 0,
            .count = kCount,
        };

        const auto gpu_result =
            run_convert_on_gpu(gpu::shaders::convert_cs16_cf32(), packed, params, 64);

        std::vector<dsp::Complex32> cpu_result(kRingSamples, dsp::Complex32{});
        REQUIRE(dsp::reference_convert_cs16_cf32(packed, params, cpu_result).has_value());

        const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
        INFO(comparison.report);
        CHECK(comparison.identical);
    }
}

TEST_CASE("conversion is bit-exact at every workgroup size and from any source offset",
          "[gpu][convert][m1]") {
    REVENANT_NEEDS_GPU();
    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x434F4E5600000003ULL;
    constexpr std::uint32_t kCount = 1500;

    std::vector<std::byte> packed(4096 * 2);
    for (std::size_t i = 0; i < packed.size(); ++i) {
        packed[i] = static_cast<std::byte>(static_cast<std::uint8_t>((i * 37U + 11U) & 0xFFU));
    }

    // An odd source offset is the interesting one. The kernel reads 32-bit
    // words and unpacks two 8-bit codes from each, so a sample that starts at
    // an odd index sits in the upper half of a word and the shift changes.
    for (const std::uint32_t src_offset : {0U, 1U, 2U, 3U, 17U}) {
        const dsp::ConvertParams params{
            .ring_mask = kRingMask,
            .dst_offset = 3U,
            .src_offset = src_offset,
            .count = kCount,
        };

        std::vector<dsp::Complex32> cpu_result(kRingSamples, dsp::Complex32{});
        REQUIRE(dsp::reference_convert_cu8_cf32(packed, params, cpu_result).has_value());

        for (const std::uint32_t local_size : {32U, 64U, 256U}) {
            if (local_size > context.info().max_workgroup_size_x) {
                continue;
            }
            const auto gpu_result =
                run_convert_on_gpu(gpu::shaders::convert_cu8_cf32(), packed, params, local_size);
            const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
            INFO("src_offset = " << src_offset << ", local_size_x = " << local_size);
            INFO(comparison.report);
            CHECK(comparison.identical);
        }
    }
}
