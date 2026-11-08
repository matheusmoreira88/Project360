#include "gpu/vulkan/vulkan_context.hpp"

#include <iostream>

int main() {
    const auto result = project360::gpu::vulkan::VulkanContext::create();
    if (result.status == project360::gpu::vulkan::VulkanContextStatus::loader_unavailable ||
        result.status == project360::gpu::vulkan::VulkanContextStatus::no_graphics_device) {
        std::cout << "SKIP: " << result.error << '\n';
        return 77;
    }
    if (!result) {
        std::cerr << "FAIL: " << result.error << '\n';
        return 1;
    }

    const auto& device = result.context->device_info();
    if (device.name.empty() || device.api_version == 0U) {
        std::cerr << "FAIL: Vulkan returned incomplete physical-device properties\n";
        return 1;
    }
    std::cout << "Vulkan context created on " << device.name << '\n';
    return 0;
}
