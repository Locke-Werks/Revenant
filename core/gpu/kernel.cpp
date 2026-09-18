#include "core/gpu/kernel.h"

#include <format>

namespace revenant::gpu {
namespace {

constexpr std::uint32_t kMaxPushConstantBytes = 128;

// Rounds up, which means the last workgroup runs partly out of range. Every
// Revenant kernel therefore bounds-checks its global invocation id. The
// alternative, requiring the caller to pad every buffer to a multiple of the
// workgroup size, pushes an implementation detail of the dispatch into the
// signal path.
std::uint32_t group_count(std::uint32_t invocations, std::uint32_t local_size) {
    if (local_size == 0) {
        return 0;
    }
    return (invocations + local_size - 1) / local_size;
}

}  // namespace

Expected<ComputePipeline> ComputePipeline::create(const Context& context,
                                                  const Options& options) {
    if (!context.valid()) {
        return fail("ComputePipeline::create called with an invalid context");
    }
    if (options.spirv.empty()) {
        return fail("ComputePipeline::create called with empty SPIR-V");
    }
    if (options.push_constant_bytes > kMaxPushConstantBytes) {
        return fail(std::format(
            "push constant block of {} bytes exceeds the {} bytes Vulkan guarantees",
            options.push_constant_bytes, kMaxPushConstantBytes));
    }
    if (options.local_size_x == 0) {
        return fail("ComputePipeline::create called with a workgroup size of zero");
    }
    if (options.local_size_x > context.info().max_workgroup_size_x) {
        return fail(std::format("workgroup size {} exceeds the {} this device allows in x",
                                options.local_size_x, context.info().max_workgroup_size_x));
    }

    ComputePipeline pipeline;
    pipeline.device_ = context.device();
    pipeline.buffer_count_ = options.storage_buffer_count;
    pipeline.local_size_x_ = options.local_size_x;

    std::vector<VkDescriptorSetLayoutBinding> bindings(options.storage_buffer_count);
    for (std::uint32_t i = 0; i < options.storage_buffer_count; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    set_info.pBindings = bindings.empty() ? nullptr : bindings.data();

    VkResult result =
        vkCreateDescriptorSetLayout(pipeline.device_, &set_info, nullptr, &pipeline.set_layout_);
    if (result != VK_SUCCESS) {
        pipeline.destroy();
        return fail(std::format("vkCreateDescriptorSetLayout failed ({})", result_name(result)),
                    result);
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = options.push_constant_bytes;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &pipeline.set_layout_;
    layout_info.pushConstantRangeCount = options.push_constant_bytes > 0 ? 1U : 0U;
    layout_info.pPushConstantRanges = options.push_constant_bytes > 0 ? &push_range : nullptr;

    result = vkCreatePipelineLayout(pipeline.device_, &layout_info, nullptr, &pipeline.layout_);
    if (result != VK_SUCCESS) {
        pipeline.destroy();
        return fail(std::format("vkCreatePipelineLayout failed ({})", result_name(result)),
                    result);
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = options.spirv.size() * sizeof(std::uint32_t);
    module_info.pCode = options.spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    result = vkCreateShaderModule(pipeline.device_, &module_info, nullptr, &module);
    if (result != VK_SUCCESS) {
        pipeline.destroy();
        return fail(std::format("vkCreateShaderModule failed ({})", result_name(result)), result);
    }

    // The workgroup size is specialized here rather than written into the
    // shader, so one SPIR-V module runs at whatever size the device and the
    // workload want. Grid constants follow it at ids 1 upwards.
    //
    // All of them are packed into one contiguous block because
    // VkSpecializationInfo takes a single pData and offsets into it, so the
    // values have to outlive the struct and sit next to each other.
    std::vector<std::uint32_t> constant_values;
    constant_values.reserve(1 + options.grid_constants.size());
    constant_values.push_back(pipeline.local_size_x_);
    constant_values.insert(constant_values.end(), options.grid_constants.begin(),
                           options.grid_constants.end());

    std::vector<VkSpecializationMapEntry> entries(constant_values.size());
    for (std::size_t i = 0; i < constant_values.size(); ++i) {
        entries[i].constantID = static_cast<std::uint32_t>(i);
        entries[i].offset = static_cast<std::uint32_t>(i * sizeof(std::uint32_t));
        entries[i].size = sizeof(std::uint32_t);
    }
    static_assert(kLocalSizeXConstantId == 0,
                  "the packing above assumes the workgroup size is constant id 0");

    VkSpecializationInfo spec{};
    spec.mapEntryCount = static_cast<std::uint32_t>(entries.size());
    spec.pMapEntries = entries.data();
    spec.dataSize = constant_values.size() * sizeof(std::uint32_t);
    spec.pData = constant_values.data();

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = options.entry_point;
    stage.pSpecializationInfo = &spec;

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = pipeline.layout_;

    result = vkCreateComputePipelines(pipeline.device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                      &pipeline.pipeline_);

    // The module is consumed by pipeline creation and is not needed afterwards.
    vkDestroyShaderModule(pipeline.device_, module, nullptr);

    if (result != VK_SUCCESS) {
        pipeline.destroy();
        return fail(std::format("vkCreateComputePipelines failed ({})", result_name(result)),
                    result);
    }

    return pipeline;
}

ComputePipeline::ComputePipeline(ComputePipeline&& other) noexcept
    : device_(other.device_),
      set_layout_(other.set_layout_),
      layout_(other.layout_),
      pipeline_(other.pipeline_),
      buffer_count_(other.buffer_count_),
      local_size_x_(other.local_size_x_) {
    other.device_ = VK_NULL_HANDLE;
    other.set_layout_ = VK_NULL_HANDLE;
    other.layout_ = VK_NULL_HANDLE;
    other.pipeline_ = VK_NULL_HANDLE;
}

ComputePipeline& ComputePipeline::operator=(ComputePipeline&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        set_layout_ = other.set_layout_;
        layout_ = other.layout_;
        pipeline_ = other.pipeline_;
        buffer_count_ = other.buffer_count_;
        local_size_x_ = other.local_size_x_;

        other.device_ = VK_NULL_HANDLE;
        other.set_layout_ = VK_NULL_HANDLE;
        other.layout_ = VK_NULL_HANDLE;
        other.pipeline_ = VK_NULL_HANDLE;
    }
    return *this;
}

ComputePipeline::~ComputePipeline() { destroy(); }

void ComputePipeline::destroy() noexcept {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

Expected<CommandRunner> CommandRunner::create(const Context& context) {
    if (!context.valid()) {
        return fail("CommandRunner::create called with an invalid context");
    }

    CommandRunner runner;
    runner.context_ = &context;

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                      VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = context.compute_family();

    VkResult result = vkCreateCommandPool(context.device(), &pool_info, nullptr, &runner.pool_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateCommandPool failed ({})", result_name(result)), result);
    }

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = runner.pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;

    result = vkAllocateCommandBuffers(context.device(), &alloc, &runner.command_buffer_);
    if (result != VK_SUCCESS) {
        runner.destroy();
        return fail(std::format("vkAllocateCommandBuffers failed ({})", result_name(result)),
                    result);
    }

    // Sized for the widest kernel M0 dispatches, with room to spare. A kernel
    // needing more bindings than this is a design smell worth noticing rather
    // than a pool worth growing silently.
    constexpr std::uint32_t kMaxStorageBuffers = 32;
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    size.descriptorCount = kMaxStorageBuffers;

    VkDescriptorPoolCreateInfo desc_info{};
    desc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    desc_info.maxSets = 1;
    desc_info.poolSizeCount = 1;
    desc_info.pPoolSizes = &size;

    result = vkCreateDescriptorPool(context.device(), &desc_info, nullptr,
                                    &runner.descriptor_pool_);
    if (result != VK_SUCCESS) {
        runner.destroy();
        return fail(std::format("vkCreateDescriptorPool failed ({})", result_name(result)),
                    result);
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    result = vkCreateFence(context.device(), &fence_info, nullptr, &runner.fence_);
    if (result != VK_SUCCESS) {
        runner.destroy();
        return fail(std::format("vkCreateFence failed ({})", result_name(result)), result);
    }

    return runner;
}

Status CommandRunner::run(const Dispatch& dispatch) {
    if (context_ == nullptr || dispatch.pipeline == nullptr) {
        return fail("CommandRunner::run called without a context or a pipeline");
    }
    if (dispatch.buffers.size() != dispatch.pipeline->storage_buffer_count()) {
        return fail(std::format("pipeline expects {} storage buffers but {} were bound",
                                dispatch.pipeline->storage_buffer_count(),
                                dispatch.buffers.size()));
    }

    const VkDevice device = context_->device();

    VkResult result = vkResetDescriptorPool(device, descriptor_pool_, 0);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkResetDescriptorPool failed ({})", result_name(result)), result);
    }

    VkDescriptorSetLayout layout = dispatch.pipeline->descriptor_layout();
    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = descriptor_pool_;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    result = vkAllocateDescriptorSets(device, &set_alloc, &set);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkAllocateDescriptorSets failed ({})", result_name(result)),
                    result);
    }

    std::vector<VkDescriptorBufferInfo> infos(dispatch.buffers.size());
    std::vector<VkWriteDescriptorSet> writes(dispatch.buffers.size());
    for (std::size_t i = 0; i < dispatch.buffers.size(); ++i) {
        infos[i].buffer = dispatch.buffers[i];
        infos[i].offset = 0;
        infos[i].range = VK_WHOLE_SIZE;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = static_cast<std::uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    if (!writes.empty()) {
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
    }

    if (auto began = begin_recording(); !began) {
        return began;
    }

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, dispatch.pipeline->handle());
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            dispatch.pipeline->layout(), 0, 1, &set, 0, nullptr);

    if (!dispatch.push_constants.empty()) {
        vkCmdPushConstants(command_buffer_, dispatch.pipeline->layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           static_cast<std::uint32_t>(dispatch.push_constants.size()),
                           dispatch.push_constants.data());
    }

    vkCmdDispatch(command_buffer_, dispatch.group_count_x, 1, 1);

    return submit_and_wait();
}

Status CommandRunner::copy(std::span<const BufferCopy> copies) {
    if (context_ == nullptr) {
        return fail("CommandRunner::copy called without a context");
    }
    if (copies.empty()) {
        return {};
    }

    if (auto began = begin_recording(); !began) {
        return began;
    }

    for (const auto& c : copies) {
        if (c.source == VK_NULL_HANDLE || c.destination == VK_NULL_HANDLE || c.bytes == 0) {
            return fail("CommandRunner::copy was given an incomplete copy descriptor");
        }
        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = 0;
        region.size = c.bytes;
        vkCmdCopyBuffer(command_buffer_, c.source, c.destination, 1, &region);
    }

    // One barrier covering the whole batch. The copies are independent of each
    // other and only need to be visible to whatever runs next, so ordering them
    // individually would buy nothing.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_HOST_READ_BIT;

    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);

    return submit_and_wait();
}

Status CommandRunner::clear(std::span<const BufferClear> buffers) {
    if (context_ == nullptr) {
        return fail("CommandRunner::clear called without a context");
    }
    if (buffers.empty()) {
        return {};
    }

    if (auto began = begin_recording(); !began) {
        return began;
    }

    for (const auto& target : buffers) {
        if (target.buffer == VK_NULL_HANDLE || target.bytes == 0) {
            return fail("CommandRunner::clear was given an incomplete target");
        }
        vkCmdFillBuffer(command_buffer_, target.buffer, 0, target.bytes, 0U);
    }

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         1, &barrier, 0, nullptr, 0, nullptr);

    return submit_and_wait();
}

Status CommandRunner::begin_recording() {
    VkResult result = vkResetCommandBuffer(command_buffer_, 0);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkResetCommandBuffer failed ({})", result_name(result)), result);
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = vkBeginCommandBuffer(command_buffer_, &begin);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkBeginCommandBuffer failed ({})", result_name(result)), result);
    }
    return {};
}

Status CommandRunner::submit_and_wait() {
    const VkDevice device = context_->device();

    VkResult result = vkEndCommandBuffer(command_buffer_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkEndCommandBuffer failed ({})", result_name(result)), result);
    }

    result = vkResetFences(device, 1, &fence_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkResetFences failed ({})", result_name(result)), result);
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;

    result = vkQueueSubmit(context_->compute_queue(), 1, &submit, fence_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkQueueSubmit failed ({})", result_name(result)), result);
    }

    // A generous but finite wait. An infinite wait on a hung GPU takes the test
    // runner down with it and reports nothing, which is strictly worse than a
    // timeout that names the kernel that hung.
    constexpr std::uint64_t kTimeoutNs = 30ULL * 1000ULL * 1000ULL * 1000ULL;
    result = vkWaitForFences(device, 1, &fence_, VK_TRUE, kTimeoutNs);
    if (result == VK_TIMEOUT) {
        return fail("GPU work did not complete within 30 seconds", result);
    }
    if (result != VK_SUCCESS) {
        return fail(std::format("vkWaitForFences failed ({})", result_name(result)), result);
    }

    return {};
}

CommandRunner::CommandRunner(CommandRunner&& other) noexcept
    : context_(other.context_),
      pool_(other.pool_),
      command_buffer_(other.command_buffer_),
      descriptor_pool_(other.descriptor_pool_),
      fence_(other.fence_) {
    other.context_ = nullptr;
    other.pool_ = VK_NULL_HANDLE;
    other.command_buffer_ = VK_NULL_HANDLE;
    other.descriptor_pool_ = VK_NULL_HANDLE;
    other.fence_ = VK_NULL_HANDLE;
}

CommandRunner& CommandRunner::operator=(CommandRunner&& other) noexcept {
    if (this != &other) {
        destroy();
        context_ = other.context_;
        pool_ = other.pool_;
        command_buffer_ = other.command_buffer_;
        descriptor_pool_ = other.descriptor_pool_;
        fence_ = other.fence_;

        other.context_ = nullptr;
        other.pool_ = VK_NULL_HANDLE;
        other.command_buffer_ = VK_NULL_HANDLE;
        other.descriptor_pool_ = VK_NULL_HANDLE;
        other.fence_ = VK_NULL_HANDLE;
    }
    return *this;
}

CommandRunner::~CommandRunner() { destroy(); }

void CommandRunner::destroy() noexcept {
    if (context_ == nullptr) {
        return;
    }
    const VkDevice device = context_->device();
    if (device == VK_NULL_HANDLE) {
        context_ = nullptr;
        return;
    }
    if (fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence_, nullptr);
        fence_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    if (pool_ != VK_NULL_HANDLE) {
        // Frees the command buffer allocated from it.
        vkDestroyCommandPool(device, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    command_buffer_ = VK_NULL_HANDLE;
    context_ = nullptr;
}

Status run_kernel(const Context& context, const KernelInvocation& invocation) {
    if (invocation.invocations == 0) {
        return fail("run_kernel called with zero invocations");
    }

    const std::size_t binding_count = invocation.inputs.size() + invocation.outputs.size();
    if (binding_count == 0) {
        return fail("run_kernel called with no buffers to bind");
    }

    auto pipeline = ComputePipeline::create(
        context, ComputePipeline::Options{
                     .spirv = invocation.spirv,
                     .storage_buffer_count = static_cast<std::uint32_t>(binding_count),
                     .local_size_x = invocation.local_size_x,
                     .push_constant_bytes =
                         static_cast<std::uint32_t>(invocation.push_constants.size()),
                     .entry_point = "main",
                     .grid_constants = invocation.grid_constants,
                 });
    if (!pipeline) {
        return std::unexpected(with_context(pipeline.error(), "run_kernel"));
    }

    constexpr VkBufferUsageFlags kDeviceUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    std::vector<Buffer> device_buffers;
    std::vector<Buffer> staging;
    std::vector<VkBuffer> handles;
    device_buffers.reserve(binding_count);
    staging.reserve(binding_count);
    handles.reserve(binding_count);

    // Inputs first, then outputs. Every Revenant kernel binds in that order.
    for (const auto& input : invocation.inputs) {
        if (input.empty()) {
            return fail("run_kernel was given an empty input buffer");
        }
        auto device_buffer = Buffer::create(context, input.size(), kDeviceUsage,
                                            MemoryKind::DeviceLocal);
        if (!device_buffer) {
            return std::unexpected(with_context(device_buffer.error(), "run_kernel input"));
        }
        auto upload = Buffer::create(context, input.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     MemoryKind::Upload);
        if (!upload) {
            return std::unexpected(with_context(upload.error(), "run_kernel staging"));
        }
        if (auto wrote = upload->write(input); !wrote) {
            return std::unexpected(with_context(wrote.error(), "run_kernel staging"));
        }
        handles.push_back(device_buffer->handle());
        device_buffers.push_back(std::move(*device_buffer));
        staging.push_back(std::move(*upload));
    }

    const std::size_t first_output = device_buffers.size();
    for (const auto& output : invocation.outputs) {
        if (output.empty()) {
            return fail("run_kernel was given an empty output buffer");
        }
        auto device_buffer = Buffer::create(context, output.size(), kDeviceUsage,
                                            MemoryKind::DeviceLocal);
        if (!device_buffer) {
            return std::unexpected(with_context(device_buffer.error(), "run_kernel output"));
        }
        auto readback = Buffer::create(context, output.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       MemoryKind::Readback);
        if (!readback) {
            return std::unexpected(with_context(readback.error(), "run_kernel readback"));
        }
        handles.push_back(device_buffer->handle());
        device_buffers.push_back(std::move(*device_buffer));
        staging.push_back(std::move(*readback));
    }

    auto runner = CommandRunner::create(context);
    if (!runner) {
        return std::unexpected(with_context(runner.error(), "run_kernel"));
    }

    // Upload, dispatch and read back are three separate submissions rather than
    // one. That costs two extra queue round trips and buys a much simpler
    // barrier story, which is the right trade for code that is never on the
    // sample path. The realtime graph at M1 records one command buffer and does
    // not build on this.
    std::vector<CommandRunner::BufferCopy> uploads;
    uploads.reserve(invocation.inputs.size());
    for (std::size_t i = 0; i < invocation.inputs.size(); ++i) {
        uploads.push_back(CommandRunner::BufferCopy{.source = staging[i].handle(),
                                                    .destination = device_buffers[i].handle(),
                                                    .bytes = device_buffers[i].size()});
    }

    // Zero the outputs before dispatching.
    //
    // A kernel that writes only part of its output, which the channelizer's
    // FFT stage does because its ring is larger than one dispatch, would
    // otherwise leave the rest holding whatever the allocator last had there.
    // The diff would compare that against a zero-initialised host buffer and
    // return a different verdict on consecutive runs of the same binary. This
    // cost one flaky failure to find and is cheap to prevent.
    std::vector<CommandRunner::BufferClear> clears;
    clears.reserve(invocation.outputs.size());
    for (std::size_t i = first_output; i < device_buffers.size(); ++i) {
        clears.push_back(CommandRunner::BufferClear{.buffer = device_buffers[i].handle(),
                                                    .bytes = device_buffers[i].size()});
    }
    if (auto cleared = runner->clear(clears); !cleared) {
        return std::unexpected(with_context(cleared.error(), "run_kernel clear"));
    }

    if (auto copied = runner->copy(uploads); !copied) {
        return std::unexpected(with_context(copied.error(), "run_kernel upload"));
    }

    CommandRunner::Dispatch dispatch{};
    dispatch.pipeline = &*pipeline;
    dispatch.buffers = handles;
    dispatch.group_count_x = invocation.group_count_x != 0
                                 ? invocation.group_count_x
                                 : group_count(invocation.invocations, invocation.local_size_x);
    dispatch.push_constants = invocation.push_constants;

    if (auto ran = runner->run(dispatch); !ran) {
        return std::unexpected(with_context(ran.error(), "run_kernel dispatch"));
    }

    std::vector<CommandRunner::BufferCopy> readbacks;
    readbacks.reserve(invocation.outputs.size());
    for (std::size_t i = first_output; i < device_buffers.size(); ++i) {
        readbacks.push_back(CommandRunner::BufferCopy{.source = device_buffers[i].handle(),
                                                      .destination = staging[i].handle(),
                                                      .bytes = device_buffers[i].size()});
    }

    if (auto copied = runner->copy(readbacks); !copied) {
        return std::unexpected(with_context(copied.error(), "run_kernel readback"));
    }

    for (std::size_t i = 0; i < invocation.outputs.size(); ++i) {
        if (auto read = staging[first_output + i].read(invocation.outputs[i]); !read) {
            return std::unexpected(with_context(read.error(), "run_kernel readback"));
        }
    }

    return {};
}

}  // namespace revenant::gpu
