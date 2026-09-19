// The four things every stage of the chain does to a command buffer.
//
// These began inside core/engine/graph.cpp's anonymous namespace, which was
// right while the graph was the only file recording dispatches. It is not any
// more: core/engine/vrx_stage.cpp records into the same command buffer through
// the VrxStage seam and needs the same four. A second copy of a barrier helper
// is the kind of duplication that stays identical for a year and then does
// not, and the two copies would be recording into one command buffer.
//
// Header-only and inline, because they are a handful of Vulkan calls each and
// exist to be named rather than to be shared code in any deeper sense.

#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/error.h"
#include "core/gpu/kernel.h"

namespace revenant::engine {

[[nodiscard]] inline std::uint32_t group_count(std::uint64_t invocations,
                                               std::uint32_t local_size) {
    if (local_size == 0) {
        return 0;
    }
    const std::uint64_t groups = (invocations + local_size - 1) / local_size;
    return static_cast<std::uint32_t>(groups);
}

// The whole-buffer dependency between two stages of the chain.
//
// A global memory barrier rather than a buffer one, deliberately. The
// hardware flushes its caches either way, so scoping the barrier to a buffer
// buys nothing here and would invite the belief that two stages barriered
// against different buffers can overlap, which on every device this project
// targets they cannot.
inline void record_barrier(VkCommandBuffer commands, VkPipelineStageFlags source_stage,
                           VkAccessFlags source_access, VkPipelineStageFlags destination_stage,
                           VkAccessFlags destination_access) {
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;

    vkCmdPipelineBarrier(commands, source_stage, destination_stage, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

inline void record_dispatch(VkCommandBuffer commands, const gpu::ComputePipeline& pipeline,
                            VkDescriptorSet set, std::span<const std::byte> push_constants,
                            std::uint32_t groups) {
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0, 1,
                            &set, 0, nullptr);
    if (!push_constants.empty()) {
        vkCmdPushConstants(commands, pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           static_cast<std::uint32_t>(push_constants.size()),
                           push_constants.data());
    }
    vkCmdDispatch(commands, groups, 1, 1);
}

// Binds storage buffers to bindings 0, 1, 2 and upwards in the order given,
// which is the binding order every Revenant kernel declares.
[[nodiscard]] inline Status write_storage_set(VkDevice device, VkDescriptorSet set,
                                              std::span<const VkBuffer> buffers) {
    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        if (buffers[i] == VK_NULL_HANDLE) {
            return fail(std::format("descriptor binding {} was left unbound", i));
        }
        infos[i].buffer = buffers[i];
        infos[i].offset = 0;
        infos[i].range = VK_WHOLE_SIZE;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = static_cast<std::uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
    return {};
}

}  // namespace revenant::engine
