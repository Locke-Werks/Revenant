#include "core/gpu/context.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>

#include <vk_mem_alloc.h>

namespace revenant::gpu {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr std::uint32_t kTargetApiVersion = VK_API_VERSION_1_3;

bool layer_available(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(layers.begin(), layers.end(), [name](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, name) == 0;
    });
}

bool instance_extension_available(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> exts(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && data != nullptr) {
        std::fputs("[vulkan] ", stderr);
        std::fputs(data->pMessage != nullptr ? data->pMessage : "(no message)", stderr);
        std::fputc('\n', stderr);
    }
    // Always VK_FALSE: the callback reports, it does not abort the call.
    return VK_FALSE;
}

Expected<VkInstance> create_instance(bool validation, bool& validation_enabled) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Revenant";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "Revenant Engine";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = kTargetApiVersion;

    std::vector<const char*> layers;
    std::vector<const char*> extensions;

    validation_enabled = false;
    if (validation && layer_available(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        validation_enabled = true;
        if (instance_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;

    // MoltenVK reports itself as a non-conformant implementation and refuses to
    // be enumerated without this. Harmless where the extension is absent.
    if (instance_extension_available(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult result = vkCreateInstance(&info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateInstance failed ({})", result_name(result)), result);
    }
    return instance;
}

DeviceInfo describe(VkPhysicalDevice device, std::uint32_t index) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);

    DeviceInfo info;
    info.index = index;
    info.name = props.deviceName;
    info.type = props.deviceType;
    info.api_version = props.apiVersion;
    info.driver_version = props.driverVersion;
    info.vendor_id = props.vendorID;
    info.device_id = props.deviceID;
    info.max_workgroup_size_x = props.limits.maxComputeWorkGroupSize[0];
    info.max_workgroup_invocations = props.limits.maxComputeWorkGroupInvocations;
    return info;
}

// A dedicated compute family, where one exists, keeps DSP submissions off the
// queue the compositor is using. Where none exists, any compute-capable family
// will do; every Vulkan implementation has at least one.
std::optional<std::uint32_t> pick_compute_family(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    std::optional<std::uint32_t> fallback;
    for (std::uint32_t i = 0; i < count; ++i) {
        const bool compute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        if (!compute || families[i].queueCount == 0) {
            continue;
        }
        const bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (!graphics) {
            return i;
        }
        if (!fallback.has_value()) {
            fallback = i;
        }
    }
    return fallback;
}

int rank(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
        default: return 0;
    }
}

std::optional<int> index_from_environment() {
    const char* raw = std::getenv("REVENANT_GPU_INDEX");
    if (raw == nullptr || *raw == '\0') {
        return std::nullopt;
    }
    char* end = nullptr;
    const long value = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0' || value < 0) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

}  // namespace

const char* result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        default: return "VkResult";
    }
}

std::string DeviceInfo::vendor_name() const {
    switch (vendor_id) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
        case 0x13B5: return "ARM";
        case 0x5143: return "Qualcomm";
        case 0x106B: return "Apple";
        case 0x10005: return "Mesa";
        default: return std::format("vendor 0x{:04X}", vendor_id);
    }
}

std::string DeviceInfo::describe() const {
    const char* kind = "other";
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: kind = "discrete"; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: kind = "integrated"; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: kind = "virtual"; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: kind = "cpu"; break;
        default: break;
    }
    return std::format("[{}] {} ({}, {}, Vulkan {}.{}.{})", index, name, vendor_name(), kind,
                       VK_API_VERSION_MAJOR(api_version), VK_API_VERSION_MINOR(api_version),
                       VK_API_VERSION_PATCH(api_version));
}

Expected<std::vector<DeviceInfo>> enumerate_devices() {
    bool validation_enabled = false;
    auto instance = create_instance(false, validation_enabled);
    if (!instance) {
        return std::unexpected(with_context(instance.error(), "enumerate_devices"));
    }

    std::uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(*instance, &count, nullptr);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        vkDestroyInstance(*instance, nullptr);
        return fail(std::format("vkEnumeratePhysicalDevices failed ({})", result_name(result)),
                    result);
    }

    std::vector<VkPhysicalDevice> handles(count);
    if (count > 0) {
        result = vkEnumeratePhysicalDevices(*instance, &count, handles.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            vkDestroyInstance(*instance, nullptr);
            return fail(std::format("vkEnumeratePhysicalDevices failed ({})", result_name(result)),
                        result);
        }
    }

    std::vector<DeviceInfo> infos;
    infos.reserve(handles.size());
    for (std::uint32_t i = 0; i < handles.size(); ++i) {
        infos.push_back(describe(handles[i], i));
    }

    vkDestroyInstance(*instance, nullptr);
    return infos;
}

Expected<Context> Context::create(Options options) {
    Context ctx;

    bool validation_enabled = false;
    auto instance = create_instance(options.validation, validation_enabled);
    if (!instance) {
        return std::unexpected(with_context(instance.error(), "Context::create"));
    }
    ctx.instance_ = *instance;

    if (validation_enabled && options.enable_debug_messenger) {
        auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx.instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT mi{};
            mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debug_callback;
            // A messenger that fails to create is not a reason to fail the
            // context; it costs diagnostics, not correctness.
            (void)create_messenger(ctx.instance_, &mi, nullptr, &ctx.messenger_);
        }
    }

    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(ctx.instance_, &count, nullptr);
    if (count == 0) {
        ctx.destroy();
        return fail("no Vulkan physical devices are present");
    }
    std::vector<VkPhysicalDevice> handles(count);
    vkEnumeratePhysicalDevices(ctx.instance_, &count, handles.data());

    int wanted = options.device_index;
    if (wanted < 0) {
        if (const auto from_env = index_from_environment()) {
            wanted = *from_env;
        }
    }

    std::uint32_t chosen = 0;
    if (wanted >= 0) {
        if (static_cast<std::uint32_t>(wanted) >= handles.size()) {
            const auto limit = handles.size();
            ctx.destroy();
            return fail(std::format(
                "requested GPU index {} but only {} Vulkan device(s) are present", wanted, limit));
        }
        chosen = static_cast<std::uint32_t>(wanted);
    } else {
        int best = -1;
        for (std::uint32_t i = 0; i < handles.size(); ++i) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(handles[i], &props);
            const int score = rank(props.deviceType);
            if (score > best) {
                best = score;
                chosen = i;
            }
        }
    }

    ctx.physical_ = handles[chosen];
    ctx.info_ = describe(ctx.physical_, chosen);

    const auto family = pick_compute_family(ctx.physical_);
    if (!family.has_value()) {
        const std::string name = ctx.info_.name;
        ctx.destroy();
        return fail(std::format("device '{}' exposes no compute-capable queue family", name));
    }
    ctx.compute_family_ = *family;

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = ctx.compute_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures features{};

    // maintenance4 is required, not optional, and the reason is subtle enough
    // to be worth stating.
    //
    // Every Revenant kernel declares its workgroup size as a specialization
    // constant (local_size_x_id), which glslang compiles to the LocalSizeId
    // execution mode when targeting Vulkan 1.3. The spec requires maintenance4
    // for that mode. Without it the behaviour is not a clean failure: a driver
    // may ignore the specialized size and fall back to one invocation per
    // workgroup, so the dispatch launches a sixty-fourth of the work it thinks
    // it launched, most of the output buffer stays untouched, and the only
    // symptom is a reference diff failing for what looks like an arithmetic
    // reason. Enabling it explicitly, and refusing the device when it is
    // absent, turns that into a message that names the actual problem.
    VkPhysicalDeviceVulkan13Features available_13{};
    available_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceFeatures2 available{};
    available.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    available.pNext = &available_13;
    vkGetPhysicalDeviceFeatures2(ctx.physical_, &available);

    if (available_13.maintenance4 != VK_TRUE) {
        const std::string name = ctx.info_.name;
        ctx.destroy();
        return fail(std::format(
            "device '{}' does not support maintenance4, which Revenant requires because every "
            "kernel specializes its workgroup size",
            name));
    }

    VkPhysicalDeviceVulkan13Features enabled_13{};
    enabled_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled_13.maintenance4 = VK_TRUE;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &enabled_13;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &features;

    VkResult result = vkCreateDevice(ctx.physical_, &device_info, nullptr, &ctx.device_);
    if (result != VK_SUCCESS) {
        const std::string name = ctx.info_.name;
        ctx.destroy();
        return fail(
            std::format("vkCreateDevice failed for '{}' ({})", name, result_name(result)), result);
    }

    vkGetDeviceQueue(ctx.device_, ctx.compute_family_, 0, &ctx.compute_queue_);

    VmaAllocatorCreateInfo alloc_info{};
    alloc_info.physicalDevice = ctx.physical_;
    alloc_info.device = ctx.device_;
    alloc_info.instance = ctx.instance_;
    alloc_info.vulkanApiVersion = kTargetApiVersion;

    result = vmaCreateAllocator(&alloc_info, &ctx.allocator_);
    if (result != VK_SUCCESS) {
        ctx.destroy();
        return fail(std::format("vmaCreateAllocator failed ({})", result_name(result)), result);
    }

    return ctx;
}

Context::Context(Context&& other) noexcept
    : instance_(other.instance_),
      messenger_(other.messenger_),
      physical_(other.physical_),
      device_(other.device_),
      compute_queue_(other.compute_queue_),
      compute_family_(other.compute_family_),
      allocator_(other.allocator_),
      info_(std::move(other.info_)) {
    other.instance_ = VK_NULL_HANDLE;
    other.messenger_ = VK_NULL_HANDLE;
    other.physical_ = VK_NULL_HANDLE;
    other.device_ = VK_NULL_HANDLE;
    other.compute_queue_ = VK_NULL_HANDLE;
    other.allocator_ = VK_NULL_HANDLE;
}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        destroy();
        instance_ = other.instance_;
        messenger_ = other.messenger_;
        physical_ = other.physical_;
        device_ = other.device_;
        compute_queue_ = other.compute_queue_;
        compute_family_ = other.compute_family_;
        allocator_ = other.allocator_;
        info_ = std::move(other.info_);

        other.instance_ = VK_NULL_HANDLE;
        other.messenger_ = VK_NULL_HANDLE;
        other.physical_ = VK_NULL_HANDLE;
        other.device_ = VK_NULL_HANDLE;
        other.compute_queue_ = VK_NULL_HANDLE;
        other.allocator_ = VK_NULL_HANDLE;
    }
    return *this;
}

Context::~Context() { destroy(); }

void Context::destroy() noexcept {
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger != nullptr) {
            destroy_messenger(instance_, messenger_, nullptr);
        }
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    physical_ = VK_NULL_HANDLE;
    compute_queue_ = VK_NULL_HANDLE;
}

}  // namespace revenant::gpu
