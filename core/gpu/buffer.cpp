#include "core/gpu/buffer.h"

#include <cstring>
#include <format>

#include <vk_mem_alloc.h>

namespace revenant::gpu {
namespace {

bool host_visible(MemoryKind kind) { return kind != MemoryKind::DeviceLocal; }

const char* kind_name(MemoryKind kind) {
    switch (kind) {
        case MemoryKind::DeviceLocal: return "DeviceLocal";
        case MemoryKind::Upload: return "Upload";
        case MemoryKind::Readback: return "Readback";
    }
    return "unknown";
}

}  // namespace

Expected<Buffer> Buffer::create(const Context& context,
                                VkDeviceSize bytes,
                                VkBufferUsageFlags usage,
                                MemoryKind kind) {
    if (bytes == 0) {
        return fail("Buffer::create called with a size of zero");
    }
    if (!context.valid()) {
        return fail("Buffer::create called with an invalid context");
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = bytes;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_info{};
    switch (kind) {
        case MemoryKind::DeviceLocal:
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            break;
        case MemoryKind::Upload:
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryKind::Readback:
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
    }

    Buffer out;
    VmaAllocationInfo detail{};
    const VkResult result = vmaCreateBuffer(context.allocator(), &buffer_info, &alloc_info,
                                            &out.buffer_, &out.allocation_, &detail);
    if (result != VK_SUCCESS) {
        return fail(std::format("vmaCreateBuffer failed for {} bytes as {} ({})", bytes,
                                kind_name(kind), result_name(result)),
                    result);
    }

    out.allocator_ = context.allocator();
    out.size_ = bytes;
    out.kind_ = kind;
    out.mapped_ = detail.pMappedData;
    return out;
}

Status Buffer::write(std::span<const std::byte> source, VkDeviceSize offset) {
    if (!host_visible(kind_)) {
        return fail("Buffer::write on a DeviceLocal buffer: stage through an Upload buffer");
    }
    if (mapped_ == nullptr) {
        return fail("Buffer::write on a buffer that is not mapped");
    }
    if (offset + source.size() > size_) {
        return fail(std::format("Buffer::write of {} bytes at offset {} overruns a {}-byte buffer",
                                source.size(), offset, size_));
    }
    std::memcpy(static_cast<std::byte*>(mapped_) + offset, source.data(), source.size());

    const VkResult result = vmaFlushAllocation(allocator_, allocation_, offset, source.size());
    if (result != VK_SUCCESS) {
        return fail(std::format("vmaFlushAllocation failed ({})", result_name(result)), result);
    }
    return {};
}

Status Buffer::read(std::span<std::byte> destination, VkDeviceSize offset) const {
    if (!host_visible(kind_)) {
        return fail("Buffer::read on a DeviceLocal buffer: copy into a Readback buffer first");
    }
    if (mapped_ == nullptr) {
        return fail("Buffer::read on a buffer that is not mapped");
    }
    if (offset + destination.size() > size_) {
        return fail(std::format("Buffer::read of {} bytes at offset {} overruns a {}-byte buffer",
                                destination.size(), offset, size_));
    }

    const VkResult result =
        vmaInvalidateAllocation(allocator_, allocation_, offset, destination.size());
    if (result != VK_SUCCESS) {
        return fail(std::format("vmaInvalidateAllocation failed ({})", result_name(result)),
                    result);
    }

    std::memcpy(destination.data(), static_cast<const std::byte*>(mapped_) + offset,
                destination.size());
    return {};
}

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_(other.allocator_),
      buffer_(other.buffer_),
      allocation_(other.allocation_),
      size_(other.size_),
      kind_(other.kind_),
      mapped_(other.mapped_) {
    other.allocator_ = VK_NULL_HANDLE;
    other.buffer_ = VK_NULL_HANDLE;
    other.allocation_ = VK_NULL_HANDLE;
    other.size_ = 0;
    other.mapped_ = nullptr;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        allocator_ = other.allocator_;
        buffer_ = other.buffer_;
        allocation_ = other.allocation_;
        size_ = other.size_;
        kind_ = other.kind_;
        mapped_ = other.mapped_;

        other.allocator_ = VK_NULL_HANDLE;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.size_ = 0;
        other.mapped_ = nullptr;
    }
    return *this;
}

Buffer::~Buffer() { destroy(); }

void Buffer::destroy() noexcept {
    if (buffer_ != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
    allocator_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    size_ = 0;
    mapped_ = nullptr;
}

}  // namespace revenant::gpu
