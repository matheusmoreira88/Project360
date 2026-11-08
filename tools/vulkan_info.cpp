#include "gpu/vulkan/vulkan_context.hpp"

#include <iostream>

int main() {
    const auto result = project360::gpu::vulkan::VulkanContext::create();
    if (!result) {
        std::cerr << "Vulkan initialization failed: " << result.error << '\n';
        return 1;
    }

    const auto& device = result.context->device_info();
    std::cout << "Vulkan graphics device: " << device.name << '\n'
              << "Vendor ID: 0x" << std::hex << device.vendor_id << '\n'
              << "Device ID: 0x" << std::hex << device.device_id << '\n'
              << std::dec << "API version: " << ((device.api_version >> 22U) & 0x7fU) << '.'
              << ((device.api_version >> 12U) & 0x3ffU) << '.' << (device.api_version & 0xfffU)
              << '\n'
              << std::dec << "Graphics queue family: " << device.graphics_queue_family << '\n';
}
