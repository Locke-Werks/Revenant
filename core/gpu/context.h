// Vulkan instance, physical device selection, logical device, compute queue and
// allocator.
//
// Device selection is explicit and overridable. This is not a convenience: the
// conformance matrix runs the same suite against every device in the machine,
// and a context that silently picks "the best one" cannot be pointed at the
// integrated GPU to prove a kernel behaves identically there.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/error.h"

// Forward-declared so VMA's header does not leak into every consumer.
VK_DEFINE_HANDLE(VmaAllocator)

namespace revenant::gpu {

struct DeviceInfo {
    std::uint32_t index = 0;
    std::string name;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;

    // Largest workgroup this device will accept in x. A kernel that hardcodes
    // its local size cannot be validated against a device with a smaller limit,
    // which is one of the reasons the size is a specialization constant.
    std::uint32_t max_workgroup_size_x = 0;
    std::uint32_t max_workgroup_invocations = 0;

    // Shared memory available to one workgroup. The FFT stage keeps a whole
    // transform here, so this is what bounds the transform size that fits in a
    // single pass.
    std::uint32_t max_workgroup_shared_memory = 0;

    // The single most important number for sizing the device ring, and one
    // that differs by a factor of 512 between the two devices in the
    // development machine: the discrete card allows 1 TiB, the integrated part
    // caps both of these at 2 GiB. A ring sized from "several seconds of
    // full-rate IQ" without consulting these configures on one and fails on
    // the other, which is a CI failure on one leg only and reads as a driver
    // problem rather than an arithmetic one.
    std::uint64_t max_memory_allocation_size = 0;
    std::uint64_t max_storage_buffer_range = 0;

    [[nodiscard]] std::string vendor_name() const;
    [[nodiscard]] std::string describe() const;
};

// Enumerates every Vulkan device without keeping a context open. Used by the
// test runner and by the revenant-devices tool.
[[nodiscard]] Expected<std::vector<DeviceInfo>> enumerate_devices();

class Context {
public:
    struct Options {
        // -1 means "choose". The choice prefers a discrete GPU, then any other
        // device, and is stable for a given machine, but it is never what CI
        // relies on: the conformance legs pass an explicit index.
        int device_index = -1;

        // Defaults to on in a debug build. Validation is the difference between
        // a descriptive error and a driver hang.
        bool validation = kValidationDefault;

        bool enable_debug_messenger = kValidationDefault;

        static constexpr bool kValidationDefault =
#ifdef NDEBUG
            false;
#else
            true;
#endif
    };

    // Reads REVENANT_GPU_INDEX when options.device_index is negative, so the
    // whole suite can be aimed at one device from the environment without every
    // test growing a flag.
    [[nodiscard]] static Expected<Context> create(Options options = {});

    Context() = default;
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&& other) noexcept;
    Context& operator=(Context&& other) noexcept;

    [[nodiscard]] VkInstance instance() const { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return physical_; }
    [[nodiscard]] VkDevice device() const { return device_; }
    // Prefer submit() below. This is here for the handful of calls that take
    // a queue and do not submit, such as vkQueueWaitIdle at teardown.
    [[nodiscard]] VkQueue compute_queue() const { return compute_queue_; }

    // vkQueueSubmit, under the lock that Vulkan requires and does not provide.
    //
    // A VkQueue is externally synchronised: two threads calling vkQueueSubmit
    // on one is undefined behaviour, not a race that merely reorders work.
    // While the only submitter was the graph's recording thread that was free.
    // It stopped being free when a receiver's construction started uploading
    // its filter tables, because core/engine/graph.h permits add_vrx while the
    // engine is running, so a control thread and the recording thread can
    // reach the same queue at once.
    //
    // The lock lives here rather than in either caller because the queue does.
    // A caller-side mutex is one that the next call site has to know to take,
    // and the failure when it does not is a driver crash somewhere unrelated.
    [[nodiscard]] VkResult submit(const VkSubmitInfo& info, VkFence fence) const {
        const std::lock_guard<std::mutex> guard(*submit_lock_);
        return vkQueueSubmit(compute_queue_, 1, &info, fence);
    }
    [[nodiscard]] std::uint32_t compute_family() const { return compute_family_; }
    [[nodiscard]] VmaAllocator allocator() const { return allocator_; }
    [[nodiscard]] const DeviceInfo& info() const { return info_; }
    [[nodiscard]] bool valid() const { return device_ != VK_NULL_HANDLE; }

private:
    void destroy() noexcept;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue compute_queue_ = VK_NULL_HANDLE;
    std::uint32_t compute_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    DeviceInfo info_{};

    // By pointer so that Context stays movable, which it has to be because
    // create() returns one by value and the engine holds it as a member.
    std::unique_ptr<std::mutex> submit_lock_ = std::make_unique<std::mutex>();
};

// Renders a VkResult as its enum name where known. Vulkan has no API for this
// and a bare negative integer in an error message costs a reader a search.
[[nodiscard]] const char* result_name(VkResult result);

}  // namespace revenant::gpu
