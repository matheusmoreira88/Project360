#include "gpu/vulkan/vulkan_context.hpp"

#include <volk.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace project360::gpu::vulkan {

struct VulkanContext::Impl {
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue graphics_queue{VK_NULL_HANDLE};
    VulkanDeviceInfo info;

    ~Impl() {
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }
};

namespace {

VulkanContextResult failure(VulkanContextStatus status, std::string message) {
    return {nullptr, status, std::move(message)};
}

std::string result_message(const char* operation, VkResult result) {
    return std::string(operation) + " failed with VkResult " + std::to_string(result);
}

bool find_graphics_queue(VkPhysicalDevice physical_device, std::uint32_t& family_index) {
    std::uint32_t family_count = 0U;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
    if (family_count == 0U) {
        return false;
    }

    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
    const auto returned_count = std::min<std::size_t>(family_count, families.size());
    const auto graphics_family = std::find_if(
        families.begin(), families.begin() + static_cast<std::ptrdiff_t>(returned_count),
        [](const VkQueueFamilyProperties& family) {
            return family.queueCount > 0U && (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U;
        });
    if (graphics_family == families.end()) {
        return false;
    }

    family_index = static_cast<std::uint32_t>(std::distance(families.begin(), graphics_family));
    return true;
}

} // namespace

VulkanContext::VulkanContext(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

VulkanContext::~VulkanContext() = default;

VulkanContextResult VulkanContext::create() {
    const auto loader_result = volkInitialize();
    if (loader_result != VK_SUCCESS) {
        return failure(VulkanContextStatus::loader_unavailable,
                       "Vulkan loader could not be initialized: " + std::to_string(loader_result));
    }

    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Project360";
    application_info.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application_info.pEngineName = "Project360";
    application_info.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;

    auto impl = std::make_unique<Impl>();
    auto result = vkCreateInstance(&instance_info, nullptr, &impl->instance);
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
        return failure(VulkanContextStatus::no_graphics_device,
                       "No Vulkan 1.0 compatible driver is available");
    }
    if (result != VK_SUCCESS) {
        return failure(VulkanContextStatus::api_error, result_message("vkCreateInstance", result));
    }
    volkLoadInstance(impl->instance);

    std::uint32_t physical_device_count = 0U;
    result = vkEnumeratePhysicalDevices(impl->instance, &physical_device_count, nullptr);
    if (result != VK_SUCCESS) {
        return failure(VulkanContextStatus::api_error,
                       result_message("vkEnumeratePhysicalDevices", result));
    }
    if (physical_device_count == 0U) {
        return failure(VulkanContextStatus::no_graphics_device,
                       "The Vulkan loader found no physical devices");
    }

    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    result =
        vkEnumeratePhysicalDevices(impl->instance, &physical_device_count, physical_devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return failure(VulkanContextStatus::api_error,
                       result_message("vkEnumeratePhysicalDevices", result));
    }

    struct Candidate {
        VkPhysicalDevice physical_device{VK_NULL_HANDLE};
        VkPhysicalDeviceProperties properties{};
        std::uint32_t graphics_queue_family{};
        int score{-1};
    };
    Candidate selected;
    const auto enumerated_count =
        std::min<std::size_t>(physical_device_count, physical_devices.size());
    for (std::size_t i = 0U; i < enumerated_count; ++i) {
        Candidate candidate;
        candidate.physical_device = physical_devices[i];
        vkGetPhysicalDeviceProperties(candidate.physical_device, &candidate.properties);
        if (!find_graphics_queue(candidate.physical_device, candidate.graphics_queue_family)) {
            continue;
        }
        candidate.score =
            candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 2
            : candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                        : 0;
        if (candidate.score > selected.score) {
            selected = candidate;
        }
    }
    if (selected.physical_device == VK_NULL_HANDLE) {
        return failure(VulkanContextStatus::no_graphics_device,
                       "No Vulkan physical device exposes a graphics queue");
    }

    constexpr float queue_priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = selected.graphics_queue_family;
    queue_info.queueCount = 1U;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1U;
    device_info.pQueueCreateInfos = &queue_info;

    impl->physical_device = selected.physical_device;
    result = vkCreateDevice(impl->physical_device, &device_info, nullptr, &impl->device);
    if (result != VK_SUCCESS) {
        return failure(VulkanContextStatus::api_error, result_message("vkCreateDevice", result));
    }
    vkGetDeviceQueue(impl->device, selected.graphics_queue_family, 0U, &impl->graphics_queue);
    if (impl->graphics_queue == VK_NULL_HANDLE) {
        return failure(VulkanContextStatus::api_error,
                       "vkGetDeviceQueue returned a null graphics queue");
    }

    impl->info.name = selected.properties.deviceName;
    impl->info.vendor_id = selected.properties.vendorID;
    impl->info.device_id = selected.properties.deviceID;
    impl->info.api_version = selected.properties.apiVersion;
    impl->info.graphics_queue_family = selected.graphics_queue_family;

    auto context = std::unique_ptr<VulkanContext>(new VulkanContext(std::move(impl)));
    return {std::move(context), VulkanContextStatus::ready, {}};
}

const VulkanDeviceInfo& VulkanContext::device_info() const noexcept {
    return impl_->info;
}

} // namespace project360::gpu::vulkan
