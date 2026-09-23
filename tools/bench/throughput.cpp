#include "tools/bench/throughput.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <new>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/dsp/pfb_branch_reference.h"
#include "core/dsp/pfb_fft_reference.h"
#include "core/engine/engine.h"
#include "core/engine/graph.h"
#include "core/engine/record_util.h"
#include "core/engine/vrx.h"
#include "core/engine/vrx_stage.h"
#include "core/gpu/buffer.h"
#include "core/gpu/context.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "core/source/registry.h"
#include "core/source/source.h"

namespace revenant::bench {
namespace {

using Clock = std::chrono::steady_clock;

constexpr VkBufferUsageFlags kDeviceStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

constexpr VkBufferUsageFlags kReadbackUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

constexpr std::uint32_t kComplexBytes = 8;

[[nodiscard]] double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// The FFT push block, byte for byte as core/shaders/pfb_fft.comp declares it.
// The same four fields core/engine/graph.cpp builds; duplicated here because
// the graph's copy is inside its implementation file and a rig that guessed
// the layout would dispatch garbage that still ran.
struct FftPushConstants {
    std::uint32_t block_base = 0;
    std::uint32_t block_count = 0;
    std::uint32_t out_ring_blocks = 0;
    std::uint32_t out_ring_mask = 0;
};

static_assert(sizeof(FftPushConstants) == 4 * sizeof(std::uint32_t),
              "the FFT push block is four packed uint32 in core/shaders/pfb_fft.comp");

[[nodiscard]] double median_of(std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 1) {
        return values[middle];
    }
    return 0.5 * (values[middle - 1] + values[middle]);
}

// ---------------------------------------------------------------------------
// The device rig
// ---------------------------------------------------------------------------

struct RigOptions {
    dsp::GridParams grid;
    dsp::SampleRate source_rate = 0;
    dsp::SampleRate audio_rate = 48'000;
    std::uint32_t blocks = 0;
    std::uint32_t receivers = 0;
    engine::Demod demod = engine::Demod::Nfm;
    dsp::Hertz bandwidth = 16'000;
};

struct RigTiming {
    double branch_us = 0.0;
    double fft_us = 0.0;
    double receivers_us = 0.0;
    double total_us = 0.0;
    double barrier_control_us = 0.0;

    // The fastest iteration, kept beside the median because the two disagree
    // by a third at the largest transform and a reader who is only shown one
    // of them cannot tell that.
    double branch_us_best = 0.0;
    double fft_us_best = 0.0;
};

// The coarse chain plus N receiver stages, recorded into one command buffer
// and submitted on its own.
//
// It is not the graph. It has no ring cursors, no staging upload, no
// completion thread and no source: those are the host-side costs the
// end-to-end pass measures, and including them here would confuse two
// different questions. What it does have is the graph's exact dispatch
// sequence for the device-side work, and the engine's own VrxStage for
// everything above the channel ring.
class DeviceRig {
public:
    [[nodiscard]] static Expected<std::unique_ptr<DeviceRig>> create(const gpu::Context& context,
                                                                     const RigOptions& options);

    ~DeviceRig();

    DeviceRig(const DeviceRig&) = delete;
    DeviceRig& operator=(const DeviceRig&) = delete;
    DeviceRig(DeviceRig&&) = delete;
    DeviceRig& operator=(DeviceRig&&) = delete;

    [[nodiscard]] Expected<RigTiming> measure(std::uint32_t iterations, std::uint32_t warmup);

private:
    DeviceRig() = default;

    [[nodiscard]] Status build(const gpu::Context& context, const RigOptions& options);
    [[nodiscard]] Status build_pipelines();
    [[nodiscard]] Status build_buffers();
    [[nodiscard]] Status build_receivers();
    [[nodiscard]] Status record(std::uint32_t iteration);
    [[nodiscard]] Status submit_and_wait(VkCommandBuffer commands);
    [[nodiscard]] Expected<std::vector<std::uint64_t>> read_queries(std::uint32_t count);
    [[nodiscard]] Expected<double> measure_barrier_control(std::uint32_t iterations);

    const gpu::Context* context_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    RigOptions options_{};

    std::uint32_t ring_mask_ = 0;
    std::uint32_t channel_ring_blocks_ = 0;
    std::uint32_t channel_ring_mask_ = 0;
    std::uint32_t local_size_x_ = 0;
    std::uint32_t fft_local_size_x_ = 0;
    double timestamp_period_ns_ = 0.0;

    gpu::Buffer iq_ring_;
    gpu::Buffer prototype_;
    gpu::Buffer twiddles_;
    gpu::Buffer branch_output_;
    gpu::Buffer channel_ring_;

    gpu::ComputePipeline branch_pipeline_;
    gpu::ComputePipeline fft_pipeline_;

    VkDescriptorPool descriptors_ = VK_NULL_HANDLE;
    VkDescriptorSet branch_set_ = VK_NULL_HANDLE;
    VkDescriptorSet fft_set_ = VK_NULL_HANDLE;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkCommandBuffer control_commands_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;

    struct Receiver {
        std::unique_ptr<engine::VrxStage> stage;
        gpu::Buffer readback;
        VkDeviceSize audio_bytes = 0;
    };
    std::vector<Receiver> receivers_;
};

DeviceRig::~DeviceRig() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    (void)vkQueueWaitIdle(context_->compute_queue());

    // The stages hold descriptor pools and buffers on this device, so they go
    // before the device-level objects they were allocated against.
    receivers_.clear();

    if (queries_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, queries_, nullptr);
    }
    if (fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, fence_, nullptr);
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
    }
    if (descriptors_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptors_, nullptr);
    }
}

Expected<std::unique_ptr<DeviceRig>> DeviceRig::create(const gpu::Context& context,
                                                       const RigOptions& options) {
    std::unique_ptr<DeviceRig> rig(new (std::nothrow) DeviceRig());
    if (rig == nullptr) {
        return fail("could not allocate the device rig");
    }
    if (auto built = rig->build(context, options); !built) {
        return std::unexpected(built.error());
    }
    return rig;
}

Status DeviceRig::build(const gpu::Context& context, const RigOptions& options) {
    context_ = &context;
    device_ = context.device();
    options_ = options;

    if (options_.blocks == 0) {
        return fail("the device rig was asked for a dispatch of zero blocks");
    }

    const gpu::DeviceInfo& info = context.info();
    const std::uint32_t ceiling =
        std::min(info.max_workgroup_size_x, info.max_workgroup_invocations);
    if (ceiling == 0) {
        return fail(std::format("'{}' reports a maximum workgroup size of zero", info.name));
    }
    local_size_x_ = std::min(gpu::kDefaultLocalSizeX, ceiling);
    fft_local_size_x_ = std::min({std::max(1U, options_.grid.channels / 2), ceiling, 256U});

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context.physical_device(), &properties);
    timestamp_period_ns_ = static_cast<double>(properties.limits.timestampPeriod);

    if (auto ok = build_buffers(); !ok) {
        return ok;
    }
    if (auto ok = build_pipelines(); !ok) {
        return ok;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context.compute_family();
    VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateCommandPool failed ({})", gpu::result_name(result)),
                    result);
    }

    VkCommandBuffer allocated[2]{};
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 2;
    result = vkAllocateCommandBuffers(device_, &alloc, allocated);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkAllocateCommandBuffers failed ({})", gpu::result_name(result)),
                    result);
    }
    commands_ = allocated[0];
    control_commands_ = allocated[1];

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(device_, &fence_info, nullptr, &fence_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateFence failed ({})", gpu::result_name(result)), result);
    }

    VkQueryPoolCreateInfo query_info{};
    query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_info.queryCount = 4;
    result = vkCreateQueryPool(device_, &query_info, nullptr, &queries_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateQueryPool failed ({})", gpu::result_name(result)),
                    result);
    }

    return build_receivers();
}

Status DeviceRig::build_buffers() {
    const gpu::Context& context = *context_;
    const dsp::GridParams& grid = options_.grid;

    // The branch filter reads the whole prototype's support below the first
    // sample of the dispatch, so the ring has to hold that plus the samples
    // the dispatch consumes. Power of two, because the kernel masks.
    const std::uint64_t consumed =
        static_cast<std::uint64_t>(options_.blocks) * grid.decimation + grid.prototype_length();
    const std::uint64_t ring_samples = std::bit_ceil(consumed);
    if (ring_samples > 0xFFFF'FFFFULL) {
        return fail(std::format("a dispatch of {} blocks needs a ring of {} samples and the "
                                "kernel's ring offset is 32 bits",
                                options_.blocks, ring_samples));
    }
    ring_mask_ = static_cast<std::uint32_t>(ring_samples - 1);

    // Four dispatches of channel ring, which is what core/engine/graph.cpp
    // derives from its default three frames in flight. The ratio matters to
    // what is being measured and not only to whether it fits: the transform's
    // write is channel-major at a stride of out_ring_blocks, so a rig with a
    // ring the size of one dispatch would scatter over a quarter of the
    // distance the engine does and report a bandwidth the engine never sees.
    // It is also what gives a receiver's fine filter room for the history it
    // reads below the dispatch.
    const std::uint64_t ring_wanted = static_cast<std::uint64_t>(options_.blocks) * 4;
    if (ring_wanted > 0xFFFF'FFFFULL) {
        return fail(std::format("a dispatch of {} blocks wants a channel ring of {} blocks and "
                                "the kernel's slot index is 32 bits",
                                options_.blocks, ring_wanted));
    }
    channel_ring_blocks_ = static_cast<std::uint32_t>(std::bit_ceil(ring_wanted));
    channel_ring_mask_ = channel_ring_blocks_ - 1;

    const VkDeviceSize ring_bytes = static_cast<VkDeviceSize>(ring_samples) * kComplexBytes;
    const VkDeviceSize prototype_bytes =
        static_cast<VkDeviceSize>(grid.prototype_length()) * sizeof(float);
    const VkDeviceSize twiddle_bytes =
        static_cast<VkDeviceSize>(grid.channels) * sizeof(dsp::Complex32);
    const VkDeviceSize branch_bytes =
        static_cast<VkDeviceSize>(options_.blocks) * grid.channels * kComplexBytes;
    const VkDeviceSize channel_bytes =
        static_cast<VkDeviceSize>(channel_ring_blocks_) * grid.channels * kComplexBytes;

    const std::uint64_t largest = std::max({static_cast<std::uint64_t>(ring_bytes),
                                            static_cast<std::uint64_t>(branch_bytes),
                                            static_cast<std::uint64_t>(channel_bytes)});
    if (largest > context.info().max_storage_buffer_range) {
        return fail(std::format("this rig needs a {} byte storage buffer and '{}' binds at most "
                                "{}",
                                largest, context.info().name,
                                context.info().max_storage_buffer_range));
    }

    const auto make = [&](VkDeviceSize bytes, const char* what) -> Expected<gpu::Buffer> {
        auto buffer = gpu::Buffer::create(context, bytes, kDeviceStorage,
                                          gpu::MemoryKind::DeviceLocal);
        if (!buffer) {
            return std::unexpected(with_context(buffer.error(), what));
        }
        return buffer;
    };

    auto ring = make(ring_bytes, "rig iq ring");
    if (!ring) {
        return std::unexpected(ring.error());
    }
    auto prototype_buffer = make(prototype_bytes, "rig prototype");
    if (!prototype_buffer) {
        return std::unexpected(prototype_buffer.error());
    }
    auto twiddle_buffer = make(twiddle_bytes, "rig twiddles");
    if (!twiddle_buffer) {
        return std::unexpected(twiddle_buffer.error());
    }
    auto branch_buffer = make(branch_bytes, "rig branch scratch");
    if (!branch_buffer) {
        return std::unexpected(branch_buffer.error());
    }
    auto channel_buffer = make(channel_bytes, "rig channel ring");
    if (!channel_buffer) {
        return std::unexpected(channel_buffer.error());
    }

    iq_ring_ = std::move(*ring);
    prototype_ = std::move(*prototype_buffer);
    twiddles_ = std::move(*twiddle_buffer);
    branch_output_ = std::move(*branch_buffer);
    channel_ring_ = std::move(*channel_buffer);

    // Real tables, not zeros. The kernels are branch-free over their data so
    // the timing does not depend on the values, but a zeroed twiddle table
    // would make the transform arithmetically meaningless and the next person
    // to extend this rig into a correctness check would have to notice.
    auto prototype_taps = dsp::design_prototype(grid);
    if (!prototype_taps) {
        return std::unexpected(with_context(prototype_taps.error(), "rig prototype"));
    }
    auto twiddle_table = dsp::build_twiddles(grid.channels);
    if (!twiddle_table) {
        return std::unexpected(with_context(twiddle_table.error(), "rig twiddles"));
    }

    auto runner = gpu::CommandRunner::create(context);
    if (!runner) {
        return std::unexpected(with_context(runner.error(), "rig upload"));
    }

    auto prototype_staging = gpu::Buffer::create(context, prototype_bytes,
                                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                 gpu::MemoryKind::Upload);
    if (!prototype_staging) {
        return std::unexpected(with_context(prototype_staging.error(), "rig prototype staging"));
    }
    if (auto wrote = prototype_staging->write(std::as_bytes(std::span(prototype_taps->taps)));
        !wrote) {
        return std::unexpected(with_context(wrote.error(), "rig prototype"));
    }

    auto twiddle_staging = gpu::Buffer::create(context, twiddle_bytes,
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                               gpu::MemoryKind::Upload);
    if (!twiddle_staging) {
        return std::unexpected(with_context(twiddle_staging.error(), "rig twiddle staging"));
    }
    if (auto wrote = twiddle_staging->write(std::as_bytes(std::span(*twiddle_table))); !wrote) {
        return std::unexpected(with_context(wrote.error(), "rig twiddles"));
    }

    // Zeroed for the reason core/engine/graph.cpp zeroes the channel ring: a
    // receiver reading a slot nothing has written demodulates whatever the
    // allocator left there, and on this rig that would be an unreproducible
    // input to a kernel whose timing is being reported.
    const gpu::CommandRunner::BufferClear clears[] = {
        {iq_ring_.handle(), iq_ring_.size()},
        {branch_output_.handle(), branch_output_.size()},
        {channel_ring_.handle(), channel_ring_.size()},
    };
    if (auto cleared = runner->clear(clears); !cleared) {
        return std::unexpected(with_context(cleared.error(), "rig clear"));
    }

    const gpu::CommandRunner::BufferCopy copies[] = {
        {prototype_staging->handle(), prototype_.handle(), prototype_bytes},
        {twiddle_staging->handle(), twiddles_.handle(), twiddle_bytes},
    };
    if (auto copied = runner->copy(copies); !copied) {
        return std::unexpected(with_context(copied.error(), "rig upload"));
    }
    return {};
}

Status DeviceRig::build_pipelines() {
    const dsp::GridParams& grid = options_.grid;

    const std::uint32_t branch_constants[] = {grid.channels, grid.taps_per_branch,
                                              grid.decimation};
    const std::uint32_t fft_constants[] = {grid.channels, grid.decimation,
                                           dsp::fft_stages(grid.channels)};

    {
        gpu::ComputePipeline::Options options;
        options.spirv = gpu::shaders::pfb_branch();
        options.storage_buffer_count = 3;
        options.local_size_x = local_size_x_;
        options.push_constant_bytes = sizeof(dsp::PfbBranchParams);
        options.grid_constants = branch_constants;
        auto pipeline = gpu::ComputePipeline::create(*context_, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), "rig branch pipeline"));
        }
        branch_pipeline_ = std::move(*pipeline);
    }
    {
        gpu::ComputePipeline::Options options;
        options.spirv = gpu::shaders::pfb_fft();
        options.storage_buffer_count = 3;
        options.local_size_x = fft_local_size_x_;
        options.push_constant_bytes = sizeof(FftPushConstants);
        options.grid_constants = fft_constants;
        auto pipeline = gpu::ComputePipeline::create(*context_, options);
        if (!pipeline) {
            return std::unexpected(with_context(pipeline.error(), "rig fft pipeline"));
        }
        fft_pipeline_ = std::move(*pipeline);
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 6;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    VkResult result = vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptors_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateDescriptorPool failed ({})", gpu::result_name(result)),
                    result);
    }

    const VkDescriptorSetLayout layouts[] = {branch_pipeline_.descriptor_layout(),
                                             fft_pipeline_.descriptor_layout()};
    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = descriptors_;
    set_alloc.descriptorSetCount = 2;
    set_alloc.pSetLayouts = layouts;

    VkDescriptorSet sets[2]{};
    result = vkAllocateDescriptorSets(device_, &set_alloc, sets);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkAllocateDescriptorSets failed ({})", gpu::result_name(result)),
                    result);
    }
    branch_set_ = sets[0];
    fft_set_ = sets[1];

    const VkBuffer branch_bound[] = {iq_ring_.handle(), prototype_.handle(),
                                     branch_output_.handle()};
    if (auto wrote = engine::write_storage_set(device_, branch_set_, branch_bound); !wrote) {
        return std::unexpected(with_context(wrote.error(), "rig branch set"));
    }
    const VkBuffer fft_bound[] = {branch_output_.handle(), twiddles_.handle(),
                                  channel_ring_.handle()};
    if (auto wrote = engine::write_storage_set(device_, fft_set_, fft_bound); !wrote) {
        return std::unexpected(with_context(wrote.error(), "rig fft set"));
    }
    return {};
}

Status DeviceRig::build_receivers() {
    if (options_.receivers == 0) {
        return {};
    }

    // The engine's own factory, so the fine stage, the seven detectors and
    // their two barriers apiece are the code under measurement rather than a
    // reimplementation of it.
    engine::install_default_vrx_stages();
    engine::VrxStageFactory factory = engine::installed_vrx_stage_factory();
    if (factory == nullptr) {
        return fail("no VrxStage factory is installed, so there are no receivers to measure");
    }

    const dsp::SampleRate channel_rate =
        options_.source_rate / static_cast<dsp::SampleRate>(options_.grid.decimation);

    // Spread across the band rather than stacked on one channel. Receivers on
    // one coarse channel would read the same cache lines out of the channel
    // ring and flatter the per-receiver figure.
    const dsp::Hertz reach = options_.source_rate * 45 / 100;

    receivers_.reserve(options_.receivers);
    for (std::uint32_t index = 0; index < options_.receivers; ++index) {
        engine::VrxParams params;
        params.center = -reach + static_cast<dsp::Hertz>(2 * reach * index /
                                                         std::max(1U, options_.receivers));
        params.bandwidth = options_.bandwidth;
        params.demod = options_.demod;
        params.audio_rate = options_.audio_rate;

        auto placement = engine::place(options_.grid, options_.source_rate, params);
        if (!placement) {
            return std::unexpected(
                with_context(placement.error(), std::format("rig receiver {}", index)));
        }

        engine::VrxStageRequest request;
        request.context = context_;
        request.id = engine::VrxId{index + 1};
        request.params = params;
        request.placement = *placement;
        request.grid = options_.grid;
        request.source_rate = options_.source_rate;
        request.channel_rate = channel_rate;
        request.channel_ring = channel_ring_.handle();
        request.channel_ring_bytes = channel_ring_.size();
        request.channel_ring_blocks = channel_ring_blocks_;
        request.channel_ring_mask = channel_ring_mask_;
        request.max_blocks_per_dispatch = options_.blocks;
        request.frames_in_flight = 1;
        request.local_size_x = local_size_x_;
        request.audio_rate = options_.audio_rate;

        // A raw receiver here is measured as a probe: the fine stage and the
        // Raw passthrough core/engine/probe.h runs, which is the only Raw
        // that has a stage to time. The graph's own raw tap is a buffer copy
        // and never reaches a factory.
        request.fine_stage_complex_tap = options_.demod == engine::Demod::Raw;

        auto stage = factory(request);
        if (!stage) {
            return std::unexpected(
                with_context(stage.error(), std::format("rig receiver {}", index)));
        }
        if (*stage == nullptr) {
            return fail(std::format(
                "the stage factory declined mode '{}', which the graph serves with a buffer copy "
                "of its own. Measure a demodulating mode here instead",
                engine::demod_name(options_.demod)));
        }

        Receiver receiver;
        receiver.stage = std::move(*stage);
        receiver.audio_bytes = receiver.stage->audio_bytes_for(options_.blocks);

        auto readback = gpu::Buffer::create(*context_, receiver.audio_bytes, kReadbackUsage,
                                            gpu::MemoryKind::Readback);
        if (!readback) {
            return std::unexpected(
                with_context(readback.error(), std::format("rig receiver {} readback", index)));
        }
        receiver.readback = std::move(*readback);
        receivers_.push_back(std::move(receiver));
    }
    return {};
}

Status DeviceRig::record(std::uint32_t iteration) {
    VkResult result = vkResetCommandBuffer(commands_, 0);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkResetCommandBuffer failed ({})", gpu::result_name(result)),
                    result);
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(commands_, &begin);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkBeginCommandBuffer failed ({})", gpu::result_name(result)),
                    result);
    }

    vkCmdResetQueryPool(commands_, queries_, 0, 4);
    vkCmdWriteTimestamp(commands_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);

    // The stream advances exactly as it does in the graph, so every stage sees
    // the block indices it would see in a run and its inter-block recurrence
    // is exercised rather than replayed from a standstill.
    const dsp::SampleIndex first_block =
        static_cast<dsp::SampleIndex>(iteration) * options_.blocks +
        (options_.grid.prototype_length() + options_.grid.decimation - 1) /
            options_.grid.decimation;

    dsp::PfbBranchParams branch_params;
    branch_params.ring_mask = ring_mask_;
    branch_params.base_offset =
        static_cast<std::uint32_t>((first_block * options_.grid.decimation) & ring_mask_);
    branch_params.block_count = options_.blocks;

    engine::record_dispatch(
        commands_, branch_pipeline_, branch_set_,
        std::as_bytes(std::span<const dsp::PfbBranchParams>(&branch_params, 1)),
        engine::group_count(static_cast<std::uint64_t>(options_.blocks) * options_.grid.channels,
                            local_size_x_));

    // Written before the barrier the graph already records here, so the split
    // costs no serialisation that was not going to happen anyway.
    vkCmdWriteTimestamp(commands_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
    engine::record_barrier(commands_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_READ_BIT);

    FftPushConstants fft_params;
    fft_params.block_base = static_cast<std::uint32_t>(first_block);
    fft_params.block_count = options_.blocks;
    fft_params.out_ring_blocks = channel_ring_blocks_;
    fft_params.out_ring_mask = channel_ring_mask_;

    engine::record_dispatch(commands_, fft_pipeline_, fft_set_,
                            std::as_bytes(std::span<const FftPushConstants>(&fft_params, 1)),
                            options_.blocks);

    vkCmdWriteTimestamp(commands_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 2);

    if (!receivers_.empty()) {
        engine::record_barrier(commands_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_SHADER_WRITE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

        for (Receiver& receiver : receivers_) {
            engine::StageRecord stage_record;
            stage_record.commands = commands_;
            stage_record.frame_index = 0;
            stage_record.channel_ring = channel_ring_.handle();
            stage_record.channel_ring_bytes = channel_ring_.size();
            stage_record.first_block = first_block;
            stage_record.block_count = options_.blocks;
            stage_record.audio_destination = receiver.readback.handle();
            stage_record.audio_bytes = receiver.audio_bytes;

            auto recorded = receiver.stage->record(stage_record);
            if (!recorded) {
                (void)vkEndCommandBuffer(commands_);
                return std::unexpected(with_context(recorded.error(), "rig receiver stage"));
            }
        }
    }

    vkCmdWriteTimestamp(commands_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 3);

    result = vkEndCommandBuffer(commands_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkEndCommandBuffer failed ({})", gpu::result_name(result)),
                    result);
    }
    return {};
}

Status DeviceRig::submit_and_wait(VkCommandBuffer commands) {
    VkResult result = vkResetFences(device_, 1, &fence_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkResetFences failed ({})", gpu::result_name(result)), result);
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;

    result = context_->submit(submit, fence_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkQueueSubmit failed ({})", gpu::result_name(result)), result);
    }

    // Generous rather than infinite: a hang here should end as an error with a
    // device name attached, not as a tool that never returns.
    constexpr std::uint64_t kTimeoutNs = 60ULL * 1000 * 1000 * 1000;
    result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, kTimeoutNs);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkWaitForFences failed ({})", gpu::result_name(result)), result);
    }
    return {};
}

Expected<std::vector<std::uint64_t>> DeviceRig::read_queries(std::uint32_t count) {
    std::vector<std::uint64_t> stamps(count, 0);
    const VkResult result = vkGetQueryPoolResults(
        device_, queries_, 0, count, stamps.size() * sizeof(std::uint64_t), stamps.data(),
        sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkGetQueryPoolResults failed ({})", gpu::result_name(result)),
                    result);
    }
    return stamps;
}

Expected<double> DeviceRig::measure_barrier_control(std::uint32_t iterations) {
    // 2N global memory barriers, in the order and with the access masks
    // core/engine/vrx_stage.cpp records them, and nothing between them. This
    // is the control for the prediction in that file's header: it puts a
    // number on what the seam's barriers cost before any of the receiver's
    // arithmetic is counted.
    const std::uint32_t pairs = options_.receivers;
    std::vector<double> samples;
    samples.reserve(iterations);

    for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
        VkResult result = vkResetCommandBuffer(control_commands_, 0);
        if (result != VK_SUCCESS) {
            return fail(std::format("vkResetCommandBuffer failed ({})", gpu::result_name(result)),
                        result);
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(control_commands_, &begin);
        if (result != VK_SUCCESS) {
            return fail(std::format("vkBeginCommandBuffer failed ({})", gpu::result_name(result)),
                        result);
        }

        vkCmdResetQueryPool(control_commands_, queries_, 0, 4);
        vkCmdWriteTimestamp(control_commands_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
        for (std::uint32_t pair = 0; pair < pairs; ++pair) {
            engine::record_barrier(control_commands_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_SHADER_READ_BIT);
            engine::record_barrier(control_commands_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_ACCESS_TRANSFER_READ_BIT);
        }
        vkCmdWriteTimestamp(control_commands_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);

        result = vkEndCommandBuffer(control_commands_);
        if (result != VK_SUCCESS) {
            return fail(std::format("vkEndCommandBuffer failed ({})", gpu::result_name(result)),
                        result);
        }

        if (auto ok = submit_and_wait(control_commands_); !ok) {
            return std::unexpected(ok.error());
        }
        auto stamps = read_queries(2);
        if (!stamps) {
            return std::unexpected(stamps.error());
        }
        samples.push_back(static_cast<double>((*stamps)[1] - (*stamps)[0]) *
                          timestamp_period_ns_ / 1000.0);
    }
    return median_of(samples);
}

Expected<RigTiming> DeviceRig::measure(std::uint32_t iterations, std::uint32_t warmup) {
    if (iterations == 0) {
        return fail("the device rig was asked for zero iterations");
    }

    std::vector<double> branch;
    std::vector<double> fft;
    std::vector<double> stages;
    std::vector<double> total;
    branch.reserve(iterations);
    fft.reserve(iterations);
    stages.reserve(iterations);
    total.reserve(iterations);

    const std::uint32_t passes = iterations + warmup;
    for (std::uint32_t iteration = 0; iteration < passes; ++iteration) {
        if (auto ok = record(iteration); !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = submit_and_wait(commands_); !ok) {
            return std::unexpected(ok.error());
        }
        auto stamps = read_queries(4);
        if (!stamps) {
            return std::unexpected(stamps.error());
        }
        if (iteration < warmup) {
            continue;
        }
        const auto to_us = [this](std::uint64_t from, std::uint64_t to) {
            return static_cast<double>(to - from) * timestamp_period_ns_ / 1000.0;
        };
        branch.push_back(to_us((*stamps)[0], (*stamps)[1]));
        fft.push_back(to_us((*stamps)[1], (*stamps)[2]));
        stages.push_back(to_us((*stamps)[2], (*stamps)[3]));
        total.push_back(to_us((*stamps)[0], (*stamps)[3]));
    }

    RigTiming timing;
    // median_of sorts in place, so the minimum is the front afterwards.
    timing.branch_us = median_of(branch);
    timing.fft_us = median_of(fft);
    timing.receivers_us = median_of(stages);
    timing.total_us = median_of(total);
    timing.branch_us_best = branch.empty() ? 0.0 : branch.front();
    timing.fft_us_best = fft.empty() ? 0.0 : fft.front();

    if (options_.receivers > 0) {
        auto control = measure_barrier_control(std::min(iterations, 16U));
        if (!control) {
            return std::unexpected(control.error());
        }
        timing.barrier_control_us = *control;
    }
    return timing;
}

// ---------------------------------------------------------------------------
// Timestamp support
// ---------------------------------------------------------------------------

struct TimestampSupport {
    bool supported = false;
    double period_ns = 0.0;
    std::string reason;
};

[[nodiscard]] TimestampSupport timestamp_support(const gpu::Context& context) {
    TimestampSupport support;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context.physical_device(), &properties);
    support.period_ns = static_cast<double>(properties.limits.timestampPeriod);

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context.physical_device(), &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(context.physical_device(), &family_count,
                                             families.data());

    const std::uint32_t family = context.compute_family();
    if (family >= families.size()) {
        support.reason = "the compute queue family is outside the reported set";
        return support;
    }
    if (families[family].timestampValidBits == 0) {
        support.reason = std::format(
            "queue family {} on '{}' reports zero valid timestamp bits, so a timestamp written "
            "there carries nothing",
            family, context.info().name);
        return support;
    }
    if (!(support.period_ns > 0.0)) {
        support.reason = std::format("'{}' reports a timestamp period of zero",
                                     context.info().name);
        return support;
    }
    support.supported = true;
    return support;
}

// ---------------------------------------------------------------------------
// The source ceiling and the end-to-end pass
// ---------------------------------------------------------------------------

[[nodiscard]] Expected<SourceCeiling> measure_source_ceiling(const ThroughputConfig& config) {
    SourceCeiling ceiling;
    ceiling.uri = throughput_source_uri(config);

    auto opened = source::open_source(ceiling.uri);
    if (!opened) {
        return std::unexpected(with_context(opened.error(), "the source ceiling"));
    }
    source::Source& src = **opened;

    std::atomic<std::uint64_t> samples{0};
    std::atomic<std::uint64_t> blocks{0};

    source::StreamOptions options;
    options.block_samples = config.block_samples;
    options.pace = 0.0;

    const Clock::time_point start = Clock::now();
    auto started = src.start(options, [&samples, &blocks](const source::SourceBlock& block) {
        // Touch one byte so the compiler cannot decide the block was never
        // read. A generator whose output is provably unused is a generator a
        // future optimiser is entitled to skip.
        if (!block.bytes.empty()) {
            static_cast<void>(block.bytes.front());
        }
        samples.fetch_add(block.sample_count, std::memory_order_relaxed);
        blocks.fetch_add(1, std::memory_order_relaxed);
        return Status{};
    });
    if (!started) {
        return std::unexpected(with_context(started.error(), "the source ceiling"));
    }

    while (src.running()) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    Status ended = src.stop();
    ceiling.wall_seconds = seconds_since(start);
    if (!ended) {
        return std::unexpected(with_context(ended.error(), "the source ceiling"));
    }

    ceiling.samples = samples.load(std::memory_order_relaxed);
    ceiling.blocks = blocks.load(std::memory_order_relaxed);
    return ceiling;
}

[[nodiscard]] Expected<EndToEndPoint> measure_end_to_end(const ThroughputConfig& config,
                                                         std::uint32_t receivers,
                                                         dsp::GridParams* grid_out,
                                                         dsp::SampleRate* channel_rate_out,
                                                         std::string* device_name,
                                                         std::string* device_summary) {
    engine::EngineConfig engine_config;
    engine_config.gpu_index = config.gpu_index;
    engine_config.channels = config.channels;
    engine_config.audio_rate = config.audio_rate;
    engine_config.block_samples = config.block_samples;
    engine_config.pace = 0.0;

    auto made = engine::Engine::create(engine_config);
    if (!made) {
        return std::unexpected(with_context(made.error(), "creating the engine"));
    }
    engine::Engine& eng = **made;

    const std::string uri = throughput_source_uri(config);
    if (auto opened = eng.open_source(uri); !opened) {
        return std::unexpected(with_context(opened.error(), std::format("opening '{}'", uri)));
    }

    if (grid_out != nullptr) {
        *grid_out = eng.info().grid;
    }
    if (channel_rate_out != nullptr) {
        *channel_rate_out = eng.info().channel_rate;
    }
    if (device_name != nullptr) {
        *device_name = eng.info().device.name;
    }
    if (device_summary != nullptr) {
        *device_summary = eng.info().device.describe();
    }

    const dsp::Hertz reach = eng.info().source_rate * 45 / 100;
    std::vector<engine::VrxId> ids;
    ids.reserve(receivers);
    for (std::uint32_t index = 0; index < receivers; ++index) {
        engine::VrxParams params;
        params.center =
            -reach + static_cast<dsp::Hertz>(2 * reach * index / std::max(1U, receivers));
        params.bandwidth = config.bandwidth;
        params.demod = config.demod;
        params.audio_rate = config.audio_rate;

        auto added = eng.add_vrx(params);
        if (!added) {
            return std::unexpected(
                with_context(added.error(), std::format("adding receiver {}", index + 1)));
        }
        ids.push_back(*added);
    }

    // Timed around run() and nothing else. Every pipeline, every tap table and
    // every buffer above is already built, so what is inside the clock is the
    // source thread, the graph, the queue and the completion thread. What it
    // also contains is up to one 2 ms poll of Engine::run's stop condition,
    // which is why ThroughputConfig::seconds defaults to a run of hundreds of
    // milliseconds rather than a few.
    const Clock::time_point start = Clock::now();
    Status ran = eng.run();
    const double wall = seconds_since(start);
    if (!ran) {
        return std::unexpected(with_context(ran.error(), "the run"));
    }

    EndToEndPoint point;
    point.receivers = receivers;
    point.wall_seconds = wall;

    const source::SourceStats stats = eng.source_stats();
    point.blocks = stats.blocks_delivered;
    point.samples = stats.samples_delivered;
    point.overrun_events = stats.overrun_events;
    point.samples_dropped = stats.samples_lost;

    for (const engine::VrxId id : ids) {
        auto status = eng.vrx_status(id);
        if (!status) {
            return std::unexpected(
                with_context(status.error(), std::format("reading back receiver {}", id.value)));
        }
        point.audio_frames += status->audio_samples;
    }
    return point;
}

// ---------------------------------------------------------------------------
// Printing
// ---------------------------------------------------------------------------

[[nodiscard]] std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

[[nodiscard]] std::string join_counts(const std::vector<std::uint32_t>& values) {
    std::string out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            out += ',';
        }
        out += std::format("{}", values[index]);
    }
    return out;
}

[[nodiscard]] double mib_of(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

}  // namespace

// ---------------------------------------------------------------------------
// Derived figures
// ---------------------------------------------------------------------------

double SourceCeiling::samples_per_second() const {
    return wall_seconds > 0.0 ? static_cast<double>(samples) / wall_seconds : 0.0;
}

double SourceCeiling::realtime_ratio(dsp::SampleRate rate) const {
    return rate > 0 ? samples_per_second() / static_cast<double>(rate) : 0.0;
}

double EndToEndPoint::blocks_per_second() const {
    return wall_seconds > 0.0 ? static_cast<double>(blocks) / wall_seconds : 0.0;
}

double EndToEndPoint::samples_per_second() const {
    return wall_seconds > 0.0 ? static_cast<double>(samples) / wall_seconds : 0.0;
}

double EndToEndPoint::realtime_ratio(dsp::SampleRate rate) const {
    return rate > 0 ? samples_per_second() / static_cast<double>(rate) : 0.0;
}

double EndToEndPoint::microseconds_per_block() const {
    return blocks > 0 ? wall_seconds * 1e6 / static_cast<double>(blocks) : 0.0;
}

double BandwidthPoint::fft_gigabytes_per_second() const {
    if (!(fft_us > 0.0)) {
        return 0.0;
    }
    const double bytes = static_cast<double>(fft_bytes_read + fft_bytes_written);
    return bytes / (fft_us * 1e-6) / 1e9;
}

double BandwidthPoint::fft_best_gigabytes_per_second() const {
    if (!(fft_us_best > 0.0)) {
        return 0.0;
    }
    const double bytes = static_cast<double>(fft_bytes_read + fft_bytes_written);
    return bytes / (fft_us_best * 1e-6) / 1e9;
}

double BandwidthPoint::branch_issued_gigabytes_per_second() const {
    if (!(branch_us > 0.0)) {
        return 0.0;
    }
    const double bytes = static_cast<double>(branch_bytes_issued + branch_bytes_written);
    return bytes / (branch_us * 1e-6) / 1e9;
}

double BandwidthPoint::branch_unique_gigabytes_per_second() const {
    if (!(branch_us > 0.0)) {
        return 0.0;
    }
    const double bytes = static_cast<double>(branch_bytes_unique + branch_bytes_written);
    return bytes / (branch_us * 1e-6) / 1e9;
}

Status ThroughputConfig::validate() const {
    if (rate <= 0) {
        return fail(std::format("rate is {}; it must be a positive integer number of samples per "
                                "second",
                                rate));
    }
    if (channels < 2 || !std::has_single_bit(channels)) {
        return fail(std::format("channels is {}; the channelizer needs a power of two of at "
                                "least two",
                                channels));
    }
    if (block_samples == 0) {
        return fail("block-samples is zero");
    }
    if (!(seconds > 0.0)) {
        return fail("seconds must be greater than zero");
    }
    if (receiver_counts.empty()) {
        return fail("no receiver counts to measure");
    }
    if (device_rig && iterations == 0) {
        return fail("iterations must be at least one");
    }
    if (bandwidth_pass) {
        if (transform_sizes.empty()) {
            return fail("no transform sizes to measure");
        }
        if (bandwidth_iterations == 0) {
            return fail("bandwidth-iterations must be at least one");
        }
        for (const std::uint32_t size : transform_sizes) {
            if (size < 2 || !std::has_single_bit(size)) {
                return fail(std::format("transform size {} is not a power of two of at least two",
                                        size));
            }
        }
        if (transform_bytes < (1ULL << 20)) {
            return fail("transform-bytes below a mebibyte measures launch overhead, not memory");
        }
    }
    return {};
}

std::string throughput_source_uri(const ThroughputConfig& config) {
    // duration= rather than samples= so the same string is legible next to the
    // rate, and seed= so a run is reproducible from what the report prints.
    return std::format(
        "synthetic:wideband?rate={}&emitters={}&noise={}&seed={}&duration={:.6f}", config.rate,
        config.emitters, config.noise ? "on" : "off", config.seed, config.seconds);
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

Expected<ThroughputReport> run_throughput(const ThroughputConfig& config) {
    if (auto ok = config.validate(); !ok) {
        return std::unexpected(ok.error());
    }

    ThroughputReport report;
    report.config = config;

    auto ceiling = measure_source_ceiling(config);
    if (!ceiling) {
        return std::unexpected(ceiling.error());
    }
    report.ceiling = *ceiling;

    report.end_to_end.reserve(config.receiver_counts.size());
    for (const std::uint32_t receivers : config.receiver_counts) {
        auto point = measure_end_to_end(config, receivers, &report.grid, &report.channel_rate,
                                        &report.device_name, &report.device_summary);
        if (!point) {
            return std::unexpected(with_context(
                point.error(), std::format("the end-to-end point at {} receivers", receivers)));
        }
        report.end_to_end.push_back(*point);
    }

    if (!config.device_rig && !config.bandwidth_pass) {
        return report;
    }

    gpu::Context::Options context_options;
    context_options.device_index = config.gpu_index;
    auto context = gpu::Context::create(context_options);
    if (!context) {
        return std::unexpected(with_context(context.error(), "the device rig"));
    }
    if (report.device_name.empty()) {
        report.device_name = context->info().name;
        report.device_summary = context->info().describe();
    }

    const TimestampSupport support = timestamp_support(*context);
    report.timestamps_supported = support.supported;
    report.timestamp_period_ns = support.period_ns;
    if (!support.supported) {
        report.notes.push_back(std::format(
            "no device timing: {}. The end-to-end curve above is wall clock and stands; the "
            "per-stage split and the channelizer's bandwidth are absent rather than guessed",
            support.reason));
        return report;
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context->physical_device(), &properties);
    const std::uint32_t max_groups = properties.limits.maxComputeWorkGroupCount[0];

    // The grid the rig runs, taken from what the engine settled on rather than
    // from what was asked for: the engine clamps the channel count to the
    // device's shared memory and a rig measuring a different grid would be
    // measuring a different engine.
    dsp::GridParams grid = report.grid;
    if (grid.channels == 0) {
        grid.channels = config.channels;
        grid.decimation = config.channels / 2;
    }

    if (config.device_rig) {
        const auto blocks = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(1, config.block_samples / grid.decimation));

        report.device.reserve(config.receiver_counts.size());
        for (const std::uint32_t receivers : config.receiver_counts) {
            RigOptions options;
            options.grid = grid;
            options.source_rate = config.rate;
            options.audio_rate = config.audio_rate;
            options.blocks = blocks;
            options.receivers = receivers;
            options.demod = config.demod;
            options.bandwidth = config.bandwidth;

            auto rig = DeviceRig::create(*context, options);
            if (!rig) {
                return std::unexpected(with_context(
                    rig.error(), std::format("the device rig at {} receivers", receivers)));
            }
            auto timing = (*rig)->measure(config.iterations, config.warmup);
            if (!timing) {
                return std::unexpected(with_context(
                    timing.error(), std::format("the device rig at {} receivers", receivers)));
            }

            DevicePoint point;
            point.receivers = receivers;
            point.blocks = blocks;
            point.branch_us = timing->branch_us;
            point.fft_us = timing->fft_us;
            point.receivers_us = timing->receivers_us;
            point.total_us = timing->total_us;
            point.barrier_control_us = timing->barrier_control_us;
            report.device.push_back(point);
        }
    }

    if (!config.bandwidth_pass) {
        return report;
    }

    const std::uint32_t transform_ceiling =
        dsp::max_fft_transform_size(context->info().max_workgroup_shared_memory);

    for (const std::uint32_t channels : config.transform_sizes) {
        if (channels > transform_ceiling) {
            report.notes.push_back(std::format(
                "transform size {} needs {} bytes of shared memory and '{}' offers {}, so it was "
                "not measured",
                channels, dsp::fft_shared_bytes(channels), context->info().name,
                context->info().max_workgroup_shared_memory));
            continue;
        }

        dsp::GridParams bandwidth_grid;
        bandwidth_grid.channels = channels;
        bandwidth_grid.decimation = channels / 2;
        bandwidth_grid.taps_per_branch = grid.taps_per_branch;

        // Blocks enough that the transform moves the target byte count. One
        // block is one M-point transform, reading M complex in and writing M
        // complex out, so blocks = bytes / (M * 8).
        std::uint64_t blocks =
            config.transform_bytes / (static_cast<std::uint64_t>(channels) * kComplexBytes);
        blocks = std::bit_floor(std::max<std::uint64_t>(blocks, 1));

        // The FFT dispatches one workgroup per block and the branch filter
        // ceil(blocks * M / local) groups, so both are bounded by the device's
        // workgroup count limit. Clamped rather than failed, and said so.
        const std::uint64_t branch_groups =
            (blocks * channels + gpu::kDefaultLocalSizeX - 1) / gpu::kDefaultLocalSizeX;
        if (blocks > max_groups || branch_groups > max_groups) {
            const std::uint64_t by_fft = max_groups;
            const std::uint64_t by_branch =
                static_cast<std::uint64_t>(max_groups) * gpu::kDefaultLocalSizeX / channels;
            const std::uint64_t allowed =
                std::bit_floor(std::max<std::uint64_t>(1, std::min(by_fft, by_branch)));
            report.notes.push_back(std::format(
                "at {} channels a {} MiB transform needs {} workgroups and '{}' dispatches at "
                "most {}, so the pass was cut to {} blocks ({:.1f} MiB)",
                channels, mib_of(config.transform_bytes), std::max(blocks, branch_groups),
                context->info().name, max_groups, allowed,
                mib_of(allowed * channels * kComplexBytes)));
            blocks = allowed;
        }

        RigOptions options;
        options.grid = bandwidth_grid;
        options.source_rate = config.rate;
        options.audio_rate = config.audio_rate;
        options.blocks = static_cast<std::uint32_t>(blocks);
        options.receivers = 0;

        auto rig = DeviceRig::create(*context, options);
        if (!rig) {
            report.notes.push_back(std::format(
                "the {}-channel bandwidth pass could not be built: {}", channels,
                rig.error().message));
            continue;
        }
        auto timing = (*rig)->measure(config.bandwidth_iterations, 2);
        if (!timing) {
            report.notes.push_back(std::format("the {}-channel bandwidth pass failed: {}",
                                               channels, timing.error().message));
            continue;
        }

        BandwidthPoint point;
        point.channels = channels;
        point.decimation = bandwidth_grid.decimation;
        point.blocks = options.blocks;
        point.fft_us = timing->fft_us;
        point.branch_us = timing->branch_us;
        point.fft_us_best = timing->fft_us_best;
        point.branch_us_best = timing->branch_us_best;

        const std::uint64_t complex_per_dispatch =
            static_cast<std::uint64_t>(options.blocks) * channels;

        // The transform reads one complex per (block, channel) out of the
        // branch scratch and writes one into the channel ring. The twiddle
        // table is M complex, read by every workgroup and resident in cache
        // after the first, so counting it would inflate the figure by the
        // ratio of workgroups to one.
        point.fft_bytes_read = complex_per_dispatch * kComplexBytes;
        point.fft_bytes_written = complex_per_dispatch * kComplexBytes;

        // The branch filter's two honest bounds. Every invocation issues
        // kTapsPerBranch complex loads, which is what the kernel asks for;
        // the unique samples underneath are blocks*D plus the prototype's
        // support, which is what DRAM has to supply if the cache does its job
        // perfectly. The truth is between them and the kernel is the same
        // either way, so both are printed and neither is called the answer.
        point.branch_bytes_issued =
            complex_per_dispatch * bandwidth_grid.taps_per_branch * kComplexBytes;
        point.branch_bytes_unique =
            (static_cast<std::uint64_t>(options.blocks) * bandwidth_grid.decimation +
             bandwidth_grid.prototype_length()) *
            kComplexBytes;
        point.branch_bytes_written = complex_per_dispatch * kComplexBytes;

        report.bandwidth.push_back(point);
    }

    return report;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void print_throughput(const ThroughputReport& report) {
    const ThroughputConfig& config = report.config;

    std::println("device    {}", report.device_summary.empty() ? report.device_name
                                                               : report.device_summary);
    std::println("grid      M {}  D {}  {} taps per branch, channel rate {} S/s", report.grid.channels,
                 report.grid.decimation, report.grid.taps_per_branch, report.channel_rate);
    std::println("source    {}", report.ceiling.uri);
    std::println("run       {:.3f} s of signal per point, block {} samples, receivers {}",
                 config.seconds, config.block_samples, join_counts(config.receiver_counts));
    std::println("receiver  {} at {} Hz bandwidth, audio {} S/s",
                 engine::demod_name(config.demod), config.bandwidth, config.audio_rate);
    std::println("");

    // --- the ceiling --------------------------------------------------------

    std::println("SOURCE CEILING (the scene alone, sink counts and returns)");
    std::println("  {} samples in {:.3f} s wall: {:.2f} MS/s, {:.2f}x realtime at {} S/s",
                 report.ceiling.samples, report.ceiling.wall_seconds,
                 report.ceiling.samples_per_second() / 1e6,
                 report.ceiling.realtime_ratio(config.rate), config.rate);
    if (config.emitters == 0 && !config.noise) {
        std::println("  The scene is silent on purpose. The generator is single threaded and a");
        std::println("  populated scene costs about four hundred times what a silent one does,");
        std::println("  which puts it below the engine and makes every row underneath read the");
        std::println("  generator's rate instead. Add --emitters 8 --noise and this row and the");
        std::println("  next table land on the same number, which is the demonstration.");
    } else if (report.ceiling.realtime_ratio(config.rate) > 0.0) {
        std::println("  This scene is populated, so the rows below are bounded by the generator");
        std::println("  and not by the engine. Compare them against the ratio above before");
        std::println("  reading anything into them.");
    }
    std::println("");

    // --- end to end ---------------------------------------------------------

    std::println("END TO END (wall clock around Engine::run, host included)");
    std::println("{:>9}  {:>9}  {:>10}  {:>11}  {:>10}  {:>12}  {:>9}  {:>8}", "receivers",
                 "wall s", "blocks/s", "us/block", "MS/s", "realtime", "audio", "overruns");
    for (const EndToEndPoint& point : report.end_to_end) {
        std::println("{:>9}  {:>9.3f}  {:>10.1f}  {:>11.1f}  {:>10.3f}  {:>11.3f}x  {:>9}  {:>8}",
                     point.receivers, point.wall_seconds, point.blocks_per_second(),
                     point.microseconds_per_block(), point.samples_per_second() / 1e6,
                     point.realtime_ratio(config.rate), point.audio_frames,
                     point.overrun_events);
    }

    if (!report.end_to_end.empty()) {
        const EndToEndPoint& base = report.end_to_end.front();
        if (base.receivers == 0 && base.microseconds_per_block() > 0.0) {
            std::println("");
            std::println("  Cost of a receiver, against the {} receiver row above.", base.receivers);
            std::println("{:>9}  {:>14}  {:>16}  {:>14}", "receivers", "us/block", "over baseline",
                         "us per recv");
            for (const EndToEndPoint& point : report.end_to_end) {
                if (point.receivers == 0) {
                    continue;
                }
                const double over = point.microseconds_per_block() - base.microseconds_per_block();
                std::println("{:>9}  {:>14.1f}  {:>16.1f}  {:>14.2f}", point.receivers,
                             point.microseconds_per_block(), over,
                             over / static_cast<double>(point.receivers));
            }
            std::println("");
            std::println("  The first rows of this column are noise. With one or two receivers");
            std::println("  the block is dominated by a host cost that does not depend on the");
            std::println("  receiver count at all, and a couple of microseconds of run-to-run");
            std::println("  variation in a {:.0f} us block divides into a per-receiver figure that"
                         " can",
                         base.microseconds_per_block());
            std::println("  come out at anything, including below zero. The slope is readable "
                         "from");
            std::println("  the rows where the receivers dominate.");
        }
    }
    std::println("");

    // --- device timing ------------------------------------------------------

    if (!report.timestamps_supported) {
        std::println("WHERE THE TIME GOES");
        std::println("  Not measured on this device. See the notes below.");
        std::println("");
    } else if (!report.device.empty()) {
        std::println("WHERE THE TIME GOES (GPU timestamps, median of {} iterations after {} "
                     "warmup)",
                     config.iterations, config.warmup);
        std::println("  One dispatch of {} channel blocks, which is what a {}-sample source block "
                     "produces.",
                     report.device.front().blocks, config.block_samples);
        std::println("  Every timestamp sits where the graph already records a barrier, so the "
                     "split");
        std::println("  costs no serialisation of its own. Host time is not in these numbers.");
        std::println("{:>9}  {:>11}  {:>11}  {:>13}  {:>11}  {:>13}  {:>13}", "receivers",
                     "branch us", "fft us", "receivers us", "total us", "2N barriers",
                     "us per recv");
        for (const DevicePoint& point : report.device) {
            const double per_receiver =
                point.receivers > 0 ? point.receivers_us / static_cast<double>(point.receivers)
                                    : 0.0;
            std::println("{:>9}  {:>11.1f}  {:>11.1f}  {:>13.1f}  {:>11.1f}  {:>13.1f}  {:>13.2f}",
                         point.receivers, point.branch_us, point.fft_us, point.receivers_us,
                         point.total_us, point.barrier_control_us, per_receiver);
        }
        std::println("");

        // The transform at the size the engine actually dispatches it, which
        // is not the size the bandwidth pass below uses and does not give the
        // same answer. Both are printed because quoting either alone invites
        // the wrong conclusion.
        {
            const DevicePoint& point = report.device.front();
            const std::uint64_t moved = 2ULL * point.blocks * report.grid.channels * kComplexBytes;
            if (point.fft_us > 0.0) {
                std::println("  At this dispatch size the transform moves {:.2f} MiB in {:.1f} us, "
                             "which is {:.0f} GB/s.",
                             mib_of(moved), point.fft_us,
                             static_cast<double>(moved) / (point.fft_us * 1e-6) / 1e9);
                std::println("  That is the engine's own figure and it is smaller than the one "
                             "below,");
                std::println("  because two megabytes is not enough work to saturate anything. "
                             "The");
                std::println("  bandwidth pass exists to answer the VkFFT question at VkFFT's own "
                             "size.");
            }
        }
        std::println("");
        std::println("  '2N barriers' is a control: a command buffer holding 2N global memory");
        std::println("  barriers in the order core/engine/vrx_stage.cpp records them and nothing");
        std::println("  else, submitted on its own. Read it for what it is. It says what it");
        std::println("  costs to issue and retire 2N barriers over an empty pipeline. It does");
        std::println("  NOT say what they cost in place, because a barrier's real price is the");
        std::println("  drain of the work in front of it and there is no work in front of these.");
        std::println("");
        std::println("  The serialisation the prediction is really about is in the last column.");
        std::println("  If receivers do not overlap, the cost of one is the same whether there");
        std::println("  is one of them or a hundred, so 'us per recv' is flat in N. If they do");
        std::println("  overlap, it falls as N rises. Compare the first receiver row with the");
        std::println("  last: that ratio is the answer, and the barrier control says how much of");
        std::println("  whatever is left could possibly be the barriers themselves.");

        const DevicePoint* first_loaded = nullptr;
        const DevicePoint* last_loaded = nullptr;
        for (const DevicePoint& point : report.device) {
            if (point.receivers == 0) {
                continue;
            }
            if (first_loaded == nullptr) {
                first_loaded = &point;
            }
            last_loaded = &point;
        }
        if (first_loaded != nullptr && last_loaded != nullptr &&
            first_loaded != last_loaded && first_loaded->receivers > 0) {
            const double first_each =
                first_loaded->receivers_us / static_cast<double>(first_loaded->receivers);
            const double last_each =
                last_loaded->receivers_us / static_cast<double>(last_loaded->receivers);
            if (first_each > 0.0) {
                std::println("");
                std::println("  {} receivers cost {:.2f} us each and {} cost {:.2f} us each, a "
                             "ratio of {:.3f}.",
                             first_loaded->receivers, first_each, last_loaded->receivers,
                             last_each, last_each / first_each);
            }
        }
        std::println("");
    }

    // The host's share, which is the gap between the two measurements above
    // and is not visible in either one alone.
    if (report.timestamps_supported && !report.device.empty() && !report.end_to_end.empty() &&
        report.end_to_end.front().receivers == 0) {
        std::println("WHAT THE HOST ADDS (end to end per block, less the device rig's total)");
        std::println("{:>9}  {:>14}  {:>14}  {:>14}", "receivers", "wall us/block",
                     "device us", "host us");
        for (const EndToEndPoint& point : report.end_to_end) {
            const DevicePoint* device = nullptr;
            for (const DevicePoint& candidate : report.device) {
                if (candidate.receivers == point.receivers) {
                    device = &candidate;
                    break;
                }
            }
            if (device == nullptr) {
                continue;
            }
            std::println("{:>9}  {:>14.1f}  {:>14.1f}  {:>14.1f}", point.receivers,
                         point.microseconds_per_block(), device->total_us,
                         point.microseconds_per_block() - device->total_us);
        }
        std::println("");
        std::println("  'host us' is a subtraction and is only as honest as what it subtracts.");
        std::println("  The two columns are not measured the same way and the difference can go");
        std::println("  either sign. The rig submits one dispatch and waits, so it pays the full");
        std::println("  submission latency on every one; the engine keeps three frames in flight");
        std::println("  and overlaps them, so at high receiver counts the rig's total exceeds the");
        std::println("  engine's whole per-block wall and this column goes negative. In the other");
        std::println("  direction, device work that runs while the host records the next block is");
        std::println("  counted in both columns. Read the sign and the order of magnitude, not");
        std::println("  the value.");
        std::println("");
    }

    // --- channelizer bandwidth ---------------------------------------------

    if (!report.bandwidth.empty()) {
        std::println("CHANNELIZER BANDWIDTH");
        std::println("  Byte count for the transform, stated so the GB/s can be checked:");
        std::println("    read    blocks * M * 8   the branch scratch, contiguous");
        std::println("    write   blocks * M * 8   the channel ring, strided by ring_blocks * 8");
        std::println("    total   2 * blocks * M * 8, divided by the measured FFT time");
        std::println("  The twiddle table is M * 8 bytes read by every workgroup and resident in");
        std::println("  cache after the first, so it is not counted: counting it would multiply");
        std::println("  the figure by the workgroup count.");
        std::println("");
        std::println("  Two things this GB/s is not. It counts bytes the kernel needed, not");
        std::println("  bytes DRAM moved: the write is eight bytes into a thirty-two byte sector");
        std::println("  at a stride of out_ring_blocks * 8, so unless the cache recombines the");
        std::println("  scatter across neighbouring blocks the memory system is doing up to four");
        std::println("  times the write traffic counted here. The figure is therefore a lower");
        std::println("  bound on the load, and the kernel is nearer the ceiling than it reads.");
        std::println("  And out_ring_blocks is four dispatches, as it is in the engine, so the");
        std::println("  scatter widens with the size of the pass and this is the kernel measured");
        std::println("  at VkFFT's size rather than at the engine's.");
        std::println("");
        std::println("  docs/fft.md puts VkFFT at {:.0f} GB/s on the 4090 and says to reopen the "
                     "decision",
                     kVkFftGigabytesPerSecond);
        std::println("  below about {:.0f}% of that, which is {:.0f} GB/s. That bar was measured "
                     "on one",
                     kReopenFraction * 100.0, kVkFftGigabytesPerSecond * kReopenFraction);
        std::println("  card and it is a property of that card, so the percentage column answers");
        std::println("  the reopen question on the 4090 and nowhere else. On any other device it");
        std::println("  is the ratio of this kernel to a number from a different machine, which");
        std::println("  is a fact about the two machines.");
        std::println("{:>7}  {:>9}  {:>11}  {:>10}  {:>10}  {:>10}  {:>9}  {}", "M", "blocks",
                     "moved MiB", "median us", "GB/s", "best GB/s", "% of bar", "verdict");
        for (const BandwidthPoint& point : report.bandwidth) {
            const double gbs = point.fft_gigabytes_per_second();
            const double fraction = gbs / kVkFftGigabytesPerSecond;
            std::println("{:>7}  {:>9}  {:>11.1f}  {:>10.1f}  {:>10.1f}  {:>10.1f}  {:>8.1f}%  {}",
                         point.channels, point.blocks,
                         mib_of(point.fft_bytes_read + point.fft_bytes_written), point.fft_us, gbs,
                         point.fft_best_gigabytes_per_second(), fraction * 100.0,
                         fraction >= kReopenFraction ? "above the reopen line"
                                                     : "BELOW the reopen line");
        }
        std::println("");
        std::println("  'GB/s' is the median iteration and 'best GB/s' the fastest. Where the");
        std::println("  two are far apart the figure is not settled and one run of this tool is");
        std::println("  not an answer: the largest transform in particular moves by a third");
        std::println("  between runs on an otherwise idle machine.");
        std::println("");
        std::println("  The branch filter, for completeness. It gathers kTapsPerBranch complex");
        std::println("  samples per output at a stride of M, so what it issues and what DRAM has");
        std::println("  to supply differ by a large factor and the truth is between them.");
        std::println("{:>7}  {:>11}  {:>13}  {:>13}  {:>13}  {:>13}", "M", "branch us",
                     "issued MiB", "unique MiB", "issued GB/s", "unique GB/s");
        for (const BandwidthPoint& point : report.bandwidth) {
            std::println("{:>7}  {:>11.1f}  {:>13.1f}  {:>13.1f}  {:>13.1f}  {:>13.1f}",
                         point.channels, point.branch_us,
                         mib_of(point.branch_bytes_issued + point.branch_bytes_written),
                         mib_of(point.branch_bytes_unique + point.branch_bytes_written),
                         point.branch_issued_gigabytes_per_second(),
                         point.branch_unique_gigabytes_per_second());
        }
        std::println("");
    }

    for (const std::string& note : report.notes) {
        std::println("note: {}", note);
    }
}

std::string throughput_to_json(const ThroughputReport& report) {
    const ThroughputConfig& config = report.config;

    std::string out;
    out += "{\n";

    out += "  \"bandwidth\": [\n";
    for (std::size_t index = 0; index < report.bandwidth.size(); ++index) {
        const BandwidthPoint& point = report.bandwidth[index];
        out += "    {\n";
        out += std::format("      \"blocks\": {},\n", point.blocks);
        out += std::format("      \"branch_bytes_issued\": {},\n", point.branch_bytes_issued);
        out += std::format("      \"branch_bytes_unique\": {},\n", point.branch_bytes_unique);
        out += std::format("      \"branch_bytes_written\": {},\n", point.branch_bytes_written);
        out += std::format("      \"branch_issued_gb_s\": {:.3f},\n",
                           point.branch_issued_gigabytes_per_second());
        out += std::format("      \"branch_unique_gb_s\": {:.3f},\n",
                           point.branch_unique_gigabytes_per_second());
        out += std::format("      \"branch_us\": {:.3f},\n", point.branch_us);
        out += std::format("      \"branch_us_best\": {:.3f},\n", point.branch_us_best);
        out += std::format("      \"channels\": {},\n", point.channels);
        out += std::format("      \"decimation\": {},\n", point.decimation);
        out += std::format("      \"fft_best_gb_s\": {:.3f},\n",
                           point.fft_best_gigabytes_per_second());
        out += std::format("      \"fft_bytes_read\": {},\n", point.fft_bytes_read);
        out += std::format("      \"fft_bytes_written\": {},\n", point.fft_bytes_written);
        out += std::format("      \"fft_gb_s\": {:.3f},\n", point.fft_gigabytes_per_second());
        out += std::format("      \"fft_us\": {:.3f},\n", point.fft_us);
        out += std::format("      \"fft_us_best\": {:.3f}\n", point.fft_us_best);
        out += index + 1 == report.bandwidth.size() ? "    }\n" : "    },\n";
    }
    out += "  ],\n";

    out += "  \"config\": {\n";
    out += std::format("    \"audio_rate\": {},\n", config.audio_rate);
    out += std::format("    \"bandwidth_hz\": {},\n", config.bandwidth);
    out += std::format("    \"block_samples\": {},\n", config.block_samples);
    out += std::format("    \"channels\": {},\n", config.channels);
    out += std::format("    \"demod\": \"{}\",\n", engine::demod_name(config.demod));
    out += std::format("    \"emitters\": {},\n", config.emitters);
    out += std::format("    \"iterations\": {},\n", config.iterations);
    out += std::format("    \"noise\": {},\n", config.noise ? "true" : "false");
    out += std::format("    \"rate\": {},\n", config.rate);
    out += std::format("    \"seconds\": {:.6f},\n", config.seconds);
    out += std::format("    \"seed\": {},\n", config.seed);
    out += std::format("    \"transform_bytes\": {},\n", config.transform_bytes);
    out += std::format("    \"warmup\": {}\n", config.warmup);
    out += "  },\n";

    out += "  \"device\": [\n";
    for (std::size_t index = 0; index < report.device.size(); ++index) {
        const DevicePoint& point = report.device[index];
        out += "    {\n";
        out += std::format("      \"barrier_control_us\": {:.3f},\n", point.barrier_control_us);
        out += std::format("      \"blocks\": {},\n", point.blocks);
        out += std::format("      \"branch_us\": {:.3f},\n", point.branch_us);
        out += std::format("      \"fft_us\": {:.3f},\n", point.fft_us);
        out += std::format("      \"receivers\": {},\n", point.receivers);
        out += std::format("      \"receivers_us\": {:.3f},\n", point.receivers_us);
        out += std::format("      \"total_us\": {:.3f}\n", point.total_us);
        out += index + 1 == report.device.size() ? "    }\n" : "    },\n";
    }
    out += "  ],\n";

    out += std::format("  \"device_name\": \"{}\",\n", json_escape(report.device_name));

    out += "  \"end_to_end\": [\n";
    for (std::size_t index = 0; index < report.end_to_end.size(); ++index) {
        const EndToEndPoint& point = report.end_to_end[index];
        out += "    {\n";
        out += std::format("      \"audio_frames\": {},\n", point.audio_frames);
        out += std::format("      \"blocks\": {},\n", point.blocks);
        out += std::format("      \"blocks_per_second\": {:.3f},\n", point.blocks_per_second());
        out += std::format("      \"microseconds_per_block\": {:.3f},\n",
                           point.microseconds_per_block());
        out += std::format("      \"overrun_events\": {},\n", point.overrun_events);
        out += std::format("      \"realtime_ratio\": {:.6f},\n",
                           point.realtime_ratio(config.rate));
        out += std::format("      \"receivers\": {},\n", point.receivers);
        out += std::format("      \"samples\": {},\n", point.samples);
        out += std::format("      \"samples_dropped\": {},\n", point.samples_dropped);
        out += std::format("      \"samples_per_second\": {:.3f},\n", point.samples_per_second());
        out += std::format("      \"wall_seconds\": {:.6f}\n", point.wall_seconds);
        out += index + 1 == report.end_to_end.size() ? "    }\n" : "    },\n";
    }
    out += "  ],\n";

    out += "  \"grid\": {\n";
    out += std::format("    \"channel_rate\": {},\n", report.channel_rate);
    out += std::format("    \"channels\": {},\n", report.grid.channels);
    out += std::format("    \"decimation\": {},\n", report.grid.decimation);
    out += std::format("    \"taps_per_branch\": {}\n", report.grid.taps_per_branch);
    out += "  },\n";

    out += "  \"notes\": [\n";
    for (std::size_t index = 0; index < report.notes.size(); ++index) {
        out += std::format("    \"{}\"{}\n", json_escape(report.notes[index]),
                           index + 1 == report.notes.size() ? "" : ",");
    }
    out += "  ],\n";

    out += std::format("  \"reopen_fraction\": {:.3f},\n", kReopenFraction);
    out += std::format("  \"schema\": {},\n", kThroughputSchemaVersion);

    out += "  \"source_ceiling\": {\n";
    out += std::format("    \"blocks\": {},\n", report.ceiling.blocks);
    out += std::format("    \"realtime_ratio\": {:.6f},\n",
                       report.ceiling.realtime_ratio(config.rate));
    out += std::format("    \"samples\": {},\n", report.ceiling.samples);
    out += std::format("    \"samples_per_second\": {:.3f},\n",
                       report.ceiling.samples_per_second());
    out += std::format("    \"uri\": \"{}\",\n", json_escape(report.ceiling.uri));
    out += std::format("    \"wall_seconds\": {:.6f}\n", report.ceiling.wall_seconds);
    out += "  },\n";

    out += std::format("  \"timestamp_period_ns\": {:.6f},\n", report.timestamp_period_ns);
    out += std::format("  \"timestamps_supported\": {},\n",
                       report.timestamps_supported ? "true" : "false");
    out += std::format("  \"vkfft_bar_gb_s\": {:.1f}\n", kVkFftGigabytesPerSecond);
    out += "}\n";
    return out;
}

}  // namespace revenant::bench
