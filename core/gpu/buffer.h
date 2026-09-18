// Device and host buffers, allocated through VMA.
//
// Three kinds, because the signal path needs exactly three and no more:
//
//   DeviceLocal  The device ring and every intermediate. Never host-visible on
//                a discrete card, which is the point: this is the memory the
//                architecture promises samples do not leave.
//   Upload       Pinned, host-writable, device-readable staging. Samples cross
//                into device memory through one of these, once.
//   Readback     Pinned, device-writable, host-readable. Audio PCM, decoded
//                symbols and detection metadata come back through one of these.
//                Nothing else does.
//
// On an integrated GPU every kind may land in the same physical memory. That is
// an allocator detail and callers must not depend on it, or the code stops
// working the moment it runs on a discrete card.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <vulkan/vulkan.h>

#include "core/error.h"
#include "core/gpu/context.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace revenant::gpu {

enum class MemoryKind {
    DeviceLocal,
    Upload,
    Readback,
};

class Buffer {
public:
    [[nodiscard]] static Expected<Buffer> create(const Context& context,
                                                 VkDeviceSize bytes,
                                                 VkBufferUsageFlags usage,
                                                 MemoryKind kind);

    Buffer() = default;
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    [[nodiscard]] VkBuffer handle() const { return buffer_; }
    [[nodiscard]] VkDeviceSize size() const { return size_; }
    [[nodiscard]] MemoryKind kind() const { return kind_; }
    [[nodiscard]] bool valid() const { return buffer_ != VK_NULL_HANDLE; }

    // Host-visible kinds only. Writing to a DeviceLocal buffer is a programming
    // error and is reported as one rather than silently doing nothing.
    [[nodiscard]] Status write(std::span<const std::byte> source, VkDeviceSize offset = 0);
    [[nodiscard]] Status read(std::span<std::byte> destination, VkDeviceSize offset = 0) const;

private:
    void destroy() noexcept;

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    MemoryKind kind_ = MemoryKind::DeviceLocal;
    void* mapped_ = nullptr;
};

}  // namespace revenant::gpu
