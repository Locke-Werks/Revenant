// Compute pipeline construction and dispatch.
//
// Two layers. ComputePipeline and CommandRunner are what the engine uses: an
// explicit pipeline, explicit barriers, explicit submission. run_kernel is the
// convenience on top, and it exists for the reference diff suite, which wants
// "run this kernel over these host buffers and give me the result" without
// forty lines of Vulkan in every test case.
//
// run_kernel stages host memory in and out on every call. That is correct for
// a test and wrong for the signal path, where the whole point is that samples
// cross the bus once. It is deliberately not used outside tests and tools.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/error.h"
#include "core/gpu/buffer.h"
#include "core/gpu/context.h"

namespace revenant::gpu {

// The specialization constant id every Revenant kernel uses for its workgroup
// size in x. Hardcoding a local size in a shader makes it unrunnable on a
// device with a smaller limit and untunable on a device with a larger one, so
// the convention is that the size is always specialized at pipeline creation.
inline constexpr std::uint32_t kLocalSizeXConstantId = 0;

inline constexpr std::uint32_t kDefaultLocalSizeX = 64;

class ComputePipeline {
public:
    struct Options {
        std::span<const std::uint32_t> spirv;
        std::uint32_t storage_buffer_count = 0;
        std::uint32_t local_size_x = kDefaultLocalSizeX;
        std::uint32_t push_constant_bytes = 0;
        const char* entry_point = "main";

        // Unsigned specialization constants bound to ids 1, 2, 3 and upwards,
        // in order. Id 0 is always the workgroup size.
        //
        // The channelizer's grid parameters live here rather than in push
        // constants, and the reason is not tidiness. Specializing M, D and
        // log2(M) lets the tap loop unroll and lets the critically-sampled
        // case fold its whole phase-correction branch away at pipeline
        // creation, as an OpSpecConstantOp the driver resolves before the
        // kernel ever runs. They are also exactly the parameters that must not
        // change when a receiver is added, so putting them somewhere that
        // requires a pipeline rebuild to change states that constraint in the
        // type system rather than in a comment.
        std::span<const std::uint32_t> grid_constants;
    };

    [[nodiscard]] static Expected<ComputePipeline> create(const Context& context,
                                                          const Options& options);

    ComputePipeline() = default;
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(ComputePipeline&& other) noexcept;
    ComputePipeline& operator=(ComputePipeline&& other) noexcept;

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }
    [[nodiscard]] VkDescriptorSetLayout descriptor_layout() const { return set_layout_; }
    [[nodiscard]] std::uint32_t storage_buffer_count() const { return buffer_count_; }
    [[nodiscard]] std::uint32_t local_size_x() const { return local_size_x_; }

private:
    void destroy() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::uint32_t buffer_count_ = 0;
    std::uint32_t local_size_x_ = kDefaultLocalSizeX;
};

// A one-shot command buffer: record, submit, wait. Sufficient for tests, tools
// and anything off the sample path. The realtime graph gets its own submission
// strategy at M1 and does not build on this.
class CommandRunner {
public:
    [[nodiscard]] static Expected<CommandRunner> create(const Context& context);

    CommandRunner() = default;
    ~CommandRunner();

    CommandRunner(const CommandRunner&) = delete;
    CommandRunner& operator=(const CommandRunner&) = delete;
    CommandRunner(CommandRunner&& other) noexcept;
    CommandRunner& operator=(CommandRunner&& other) noexcept;

    struct Dispatch {
        const ComputePipeline* pipeline = nullptr;
        std::span<const VkBuffer> buffers;
        std::uint32_t group_count_x = 1;
        std::span<const std::byte> push_constants;
    };

    [[nodiscard]] Status run(const Dispatch& dispatch);

    struct BufferCopy {
        VkBuffer source = VK_NULL_HANDLE;
        VkBuffer destination = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
    };

    // Batched so that staging a whole invocation's inputs costs one submission
    // rather than one per buffer.
    [[nodiscard]] Status copy(std::span<const BufferCopy> copies);

    struct BufferClear {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
    };

    // Zeroes device buffers.
    //
    // Necessary, not hygiene. A freshly allocated device buffer holds whatever
    // was last in that memory, and a kernel that writes only part of its
    // output leaves the rest at that value. The reference diff then compares
    // the untouched region against a host buffer that was zero-initialised,
    // and fails or passes depending on what the allocator handed back. That is
    // a bit-exactness suite that reports a different answer on consecutive
    // runs of the same binary, which is worse than no suite: it trains people
    // to re-run until it goes green.
    [[nodiscard]] Status clear(std::span<const BufferClear> buffers);

private:
    void destroy() noexcept;

    [[nodiscard]] Status begin_recording();
    [[nodiscard]] Status submit_and_wait();

    const Context* context_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

struct KernelInvocation {
    std::span<const std::uint32_t> spirv;

    // Bound at bindings 0..inputs.size()-1, then outputs immediately after. A
    // shader's binding order is therefore inputs first, outputs second, which
    // every Revenant kernel follows.
    std::vector<std::span<const std::byte>> inputs;
    std::vector<std::span<std::byte>> outputs;

    std::span<const std::byte> push_constants;

    // Total invocations in x. The group count is derived by rounding up, so
    // every kernel must bounds-check its global id against a push constant or
    // a buffer length.
    std::uint32_t invocations = 0;

    std::uint32_t local_size_x = kDefaultLocalSizeX;

    // Bound to specialization constants 1, 2, 3 and upwards. See
    // ComputePipeline::Options::grid_constants.
    std::vector<std::uint32_t> grid_constants;

    // Dispatch this many workgroups instead of deriving the count from
    // invocations.
    //
    // A kernel that treats a workgroup as the unit of work rather than an
    // invocation needs this. The FFT stage is one: it holds a whole transform
    // in shared memory and indexes by gl_WorkGroupID, so the group count is
    // the number of transforms and has nothing to do with how the threads
    // within one are arranged. Deriving it from invocations would work only by
    // arithmetic coincidence and would break the moment the thread count per
    // transform changed.
    std::uint32_t group_count_x = 0;
};

// Uploads the inputs, dispatches, reads the outputs back into the caller's
// spans. Synchronous.
[[nodiscard]] Status run_kernel(const Context& context, const KernelInvocation& invocation);

}  // namespace revenant::gpu
