// M0.1 exit criterion: upload a buffer, run a trivial kernel, read it back,
// assert the result. Run on every device in the conformance matrix.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/complex_ops.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

TEST_CASE("a Vulkan device is present and can be described", "[gpu][m0.1]") {
    REVENANT_NEEDS_GPU();

    auto devices = gpu::enumerate_devices();
    REQUIRE(devices.has_value());
    REQUIRE_FALSE(devices->empty());

    // Printed so a CI log identifies what actually ran. Two devices in this
    // machine means two legs of the matrix, and a log that does not say which
    // one produced a result is not evidence of anything.
    INFO("devices visible to this process:");
    for (const auto& device : *devices) {
        INFO("  " << device.describe());
    }

    for (const auto& device : *devices) {
        CHECK(device.max_workgroup_size_x >= 64);
        CHECK(device.max_workgroup_invocations >= 64);
    }
}

TEST_CASE("the shared context selects a device and exposes a compute queue", "[gpu][m0.1]") {
    REVENANT_NEEDS_GPU();

    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    REQUIRE(context.valid());
    CHECK(context.device() != VK_NULL_HANDLE);
    CHECK(context.compute_queue() != VK_NULL_HANDLE);
    CHECK(context.allocator() != VK_NULL_HANDLE);
    CHECK_FALSE(context.info().name.empty());
}

TEST_CASE("a buffer survives a round trip through device memory", "[gpu][m0.1]") {
    REVENANT_NEEDS_GPU();

    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x5245564E414E5401ULL;
    constexpr std::size_t kCount = 4096;

    test::SeededInput input(kSeed);
    const std::vector<float> source = input.reals(kCount);
    std::vector<float> result(kCount, 0.0F);

    const auto count = static_cast<std::uint32_t>(kCount);
    const auto push = std::as_bytes(std::span<const std::uint32_t>(&count, 1));

    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::identity();
    invocation.inputs = {std::as_bytes(std::span<const float>(source))};
    invocation.outputs = {std::as_writable_bytes(std::span<float>(result))};
    invocation.push_constants = push;
    invocation.invocations = count;

    const auto ran = gpu::run_kernel(context, invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    // A copy must be exact. If this fails, nothing downstream about arithmetic
    // is worth investigating until staging is fixed.
    std::vector<float> expected(kCount, 0.0F);
    REQUIRE(dsp::reference_copy(source, expected).has_value());

    const auto comparison = test::diff(result, expected, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
}

TEST_CASE("the workgroup size is specializable rather than baked in", "[gpu][m0.1]") {
    REVENANT_NEEDS_GPU();

    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x5245564E414E5402ULL;
    constexpr std::size_t kCount = 1000;  // deliberately not a multiple of any size below

    test::SeededInput input(kSeed);
    const std::vector<float> source = input.reals(kCount);

    const auto count = static_cast<std::uint32_t>(kCount);
    const auto push = std::as_bytes(std::span<const std::uint32_t>(&count, 1));

    std::vector<float> expected(kCount, 0.0F);
    REQUIRE(dsp::reference_copy(source, expected).has_value());

    // The same module, specialized four ways. This is the property that lets
    // one SPIR-V binary run on a device with a 64-invocation limit and on one
    // with 1024, and it also exercises the bounds check in the kernel: none of
    // these sizes divides 1000, so every run has a partly out-of-range final
    // workgroup.
    for (const std::uint32_t local_size : {32U, 64U, 128U, 256U}) {
        if (local_size > context.info().max_workgroup_size_x) {
            continue;
        }

        std::vector<float> result(kCount, 0.0F);

        gpu::KernelInvocation invocation;
        invocation.spirv = gpu::shaders::identity();
        invocation.inputs = {std::as_bytes(std::span<const float>(source))};
        invocation.outputs = {std::as_writable_bytes(std::span<float>(result))};
        invocation.push_constants = push;
        invocation.invocations = count;
        invocation.local_size_x = local_size;

        const auto ran = gpu::run_kernel(context, invocation);
        INFO("local_size_x = " << local_size);
        INFO(test::message_of(ran));
        REQUIRE(ran.has_value());

        const auto comparison = test::diff(result, expected, kSeed);
        INFO(comparison.report);
        CHECK(comparison.identical);
    }
}

TEST_CASE("an oversized workgroup is refused rather than clamped", "[gpu][m0.1]") {
    REVENANT_NEEDS_GPU();

    auto& context = test::shared_context();

    gpu::ComputePipeline::Options options;
    options.spirv = gpu::shaders::identity();
    options.storage_buffer_count = 2;
    options.local_size_x = context.info().max_workgroup_size_x + 1;
    options.push_constant_bytes = sizeof(std::uint32_t);

    const auto pipeline = gpu::ComputePipeline::create(context, options);

    // Silently clamping would produce a kernel that runs and computes the wrong
    // thing. Refusing is the only safe answer.
    REQUIRE_FALSE(pipeline.has_value());
    CHECK(pipeline.error().message.find("exceeds") != std::string::npos);
}
