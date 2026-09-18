// M0.2: the complex multiply kernel against its CPU twin, bit-exactly.
//
// This is the case the rest of the project's numerical confidence rests on. It
// is not testing that complex multiplication works. It is testing that the same
// arithmetic, expressed once in GLSL and once in C++, produces identical bits
// on whichever GPU the matrix is currently pointed at.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/complex_ops.h"
#include "core/dsp/types.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// Runs cmul on the GPU and returns the result, or fails the test with the
// driver's own message.
std::vector<dsp::Complex32> run_cmul_on_gpu(std::span<const std::uint32_t> spirv,
                                            std::span<const dsp::Complex32> lhs,
                                            std::span<const dsp::Complex32> rhs,
                                            std::uint32_t local_size) {
    auto& context = test::shared_context();

    std::vector<dsp::Complex32> result(lhs.size(), dsp::Complex32{});
    const auto count = static_cast<std::uint32_t>(lhs.size());
    const auto push = std::as_bytes(std::span<const std::uint32_t>(&count, 1));

    gpu::KernelInvocation invocation;
    invocation.spirv = spirv;
    invocation.inputs = {std::as_bytes(lhs), std::as_bytes(rhs)};
    invocation.outputs = {std::as_writable_bytes(std::span<dsp::Complex32>(result))};
    invocation.push_constants = push;
    invocation.invocations = count;
    invocation.local_size_x = local_size;

    const auto ran = gpu::run_kernel(context, invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    return result;
}

std::vector<dsp::Complex32> run_cmul_on_cpu(std::span<const dsp::Complex32> lhs,
                                            std::span<const dsp::Complex32> rhs) {
    std::vector<dsp::Complex32> expected(lhs.size(), dsp::Complex32{});
    const auto computed = dsp::reference_cmul(lhs, rhs, expected);
    INFO(test::message_of(computed));
    REQUIRE(computed.has_value());
    return expected;
}

}  // namespace

TEST_CASE("cmul matches its CPU reference bit-exactly on uniform input", "[gpu][reference][m0.2]") {
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x434D554C00000001ULL;
    constexpr std::size_t kCount = 65536;

    test::SeededInput input(kSeed);
    const auto lhs = input.complexes(kCount);
    const auto rhs = input.complexes(kCount);

    const auto gpu_result = run_cmul_on_gpu(gpu::shaders::cmul(), lhs, rhs, 64);
    const auto cpu_result = run_cmul_on_cpu(lhs, rhs);

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
    CHECK(comparison.max_ulp_error == 0);
}

TEST_CASE("cmul matches its CPU reference on values chosen to provoke rounding",
          "[gpu][reference][m0.2]") {
    INFO("running on " << test::shared_context_description());

    // Uniform random input in [-1, 1] almost never distinguishes a fused
    // multiply-add from two separate operations: the intermediate product is
    // usually representable and the two agree by luck. These values do not have
    // that courtesy. Denormals, values straddling the 2^24 integer boundary,
    // and magnitudes far enough apart that the intermediate rounding is
    // visible. This is the case that would catch a missing NoContraction.
    constexpr std::uint64_t kSeed = 0x434D554C00000002ULL;
    constexpr std::size_t kCount = 16384;

    test::SeededInput input(kSeed);
    const auto lhs = input.adversarial_complexes(kCount);
    const auto rhs = input.adversarial_complexes(kCount);

    const auto gpu_result = run_cmul_on_gpu(gpu::shaders::cmul(), lhs, rhs, 64);
    const auto cpu_result = run_cmul_on_cpu(lhs, rhs);

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
    CHECK(comparison.max_ulp_error == 0);
}

TEST_CASE("cmul is bit-exact at every workgroup size", "[gpu][reference][m0.2]") {
    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x434D554C00000003ULL;
    constexpr std::size_t kCount = 12345;  // not a multiple of any size tried

    test::SeededInput input(kSeed);
    const auto lhs = input.complexes(kCount);
    const auto rhs = input.complexes(kCount);
    const auto cpu_result = run_cmul_on_cpu(lhs, rhs);

    // The workgroup size must not change the answer. It is a scheduling
    // decision, and a kernel whose output depends on it has a race or an
    // out-of-bounds access that happens to be benign at one size.
    for (const std::uint32_t local_size : {32U, 64U, 128U, 256U}) {
        if (local_size > context.info().max_workgroup_size_x) {
            continue;
        }
        const auto gpu_result = run_cmul_on_gpu(gpu::shaders::cmul(), lhs, rhs, local_size);
        const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
        INFO("local_size_x = " << local_size);
        INFO(comparison.report);
        CHECK(comparison.identical);
    }
}
