// Proof that the reference diff catches a bad kernel.
//
// A harness that has only ever been shown to pass is not evidence of anything.
// M0's exit criterion is explicitly that a deliberately wrong kernel fails the
// suite and that the reported seed reproduces it, so that is tested here rather
// than assumed.

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/complex_ops.h"
#include "core/dsp/types.h"
#include "core/gpu/kernel.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

#include "shaders/cmul_wrong_comp.h"

using namespace revenant;

TEST_CASE("the diff detects a single unit in the last place", "[reference][m0.2]") {
    // 1.0f and the next representable float above it. An epsilon comparison
    // would call these equal, which is precisely the blindness this harness
    // exists to avoid.
    const std::vector<float> a{1.0F};
    const std::vector<float> b{std::bit_cast<float>(std::bit_cast<std::uint32_t>(1.0F) + 1U)};

    const auto strict = test::diff(a, b, 1234);
    CHECK_FALSE(strict.identical);
    CHECK(strict.max_ulp_error == 1);
    CHECK(strict.first_divergence == 0);

    // The report has to carry enough to reproduce and diagnose: the seed, the
    // index, and both bit patterns.
    CHECK(strict.report.find("1234") != std::string::npos);
    CHECK(strict.report.find("0x3F800000") != std::string::npos);
    CHECK(strict.report.find("0x3F800001") != std::string::npos);

    // And a deliberately raised tolerance must actually relax it, or a block
    // that genuinely cannot be bit-exact has no way to pass.
    const auto tolerant = test::diff(a, b, 1234, 1);
    CHECK(tolerant.identical);
}

TEST_CASE("the diff distinguishes signed zero and catches size mismatch", "[reference][m0.2]") {
    // +0.0 and -0.0 compare equal with ==, and are different bit patterns that
    // propagate differently through a division or an atan2. A bitwise harness
    // must not treat them as the same value.
    const std::vector<float> positive{0.0F};
    const std::vector<float> negative{-0.0F};
    CHECK_FALSE(test::diff(positive, negative, 1).identical);

    const std::vector<float> longer{0.0F, 0.0F};
    const auto mismatch = test::diff(positive, longer, 1);
    CHECK_FALSE(mismatch.identical);
    CHECK(mismatch.report.find("size mismatch") != std::string::npos);
}

TEST_CASE("identical buffers compare identical", "[reference][m0.2]") {
    test::SeededInput input(99);
    const auto values = input.reals(1024);
    const auto same = test::diff(values, values, 99);
    CHECK(same.identical);
    CHECK(same.max_ulp_error == 0);
    CHECK(same.compared == 1024);
    CHECK(same.report.empty());
}

TEST_CASE("a deliberately wrong kernel fails the diff", "[gpu][reference][m0.2]") {
    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0xBADC0DE000000001ULL;
    constexpr std::size_t kCount = 8192;

    test::SeededInput input(kSeed);
    const auto lhs = input.complexes(kCount);
    const auto rhs = input.complexes(kCount);

    std::vector<dsp::Complex32> gpu_result(kCount, dsp::Complex32{});
    const auto count = static_cast<std::uint32_t>(kCount);
    const auto push = std::as_bytes(std::span<const std::uint32_t>(&count, 1));

    gpu::KernelInvocation invocation;
    invocation.spirv = std::span<const std::uint32_t>(cmul_wrong_comp_spv,
                                                      std::size(cmul_wrong_comp_spv));
    invocation.inputs = {std::as_bytes(std::span<const dsp::Complex32>(lhs)),
                         std::as_bytes(std::span<const dsp::Complex32>(rhs))};
    invocation.outputs = {std::as_writable_bytes(std::span<dsp::Complex32>(gpu_result))};
    invocation.push_constants = push;
    invocation.invocations = count;

    const auto ran = gpu::run_kernel(context, invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    std::vector<dsp::Complex32> cpu_result(kCount, dsp::Complex32{});
    REQUIRE(dsp::reference_cmul(lhs, rhs, cpu_result).has_value());

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);

    // The whole point. The broken kernel produces finite, plausible numbers and
    // the harness must still reject it.
    CHECK_FALSE(comparison.identical);
    CHECK(comparison.max_ulp_error > 0);
    CHECK(comparison.report.find("seed") != std::string::npos);

    // The imaginary part is correct in the broken kernel, so the first
    // divergence must be at an even index: the real component of some element.
    CHECK(comparison.first_divergence % 2 == 0);
}
