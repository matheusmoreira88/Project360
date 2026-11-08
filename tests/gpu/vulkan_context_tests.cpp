#include "gpu/vulkan/vulkan_context.hpp"

#include <array>
#include <iostream>
#include <thread>

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
    const auto pixel = result.context->readback_clear_color_pixel();
    if (!pixel) {
        std::cerr << "FAIL: " << pixel.error << '\n';
        return 1;
    }
    if (pixel.rgba != std::array<std::uint8_t, 4>{255U, 0U, 255U, 255U}) {
        std::cerr << "FAIL: offscreen clear/readback returned unexpected RGBA values\n";
        return 1;
    }

    std::array<project360::gpu::vulkan::VulkanPixelReadbackResult, 2> concurrent_pixels{};
    std::thread first_readback(
        [&] { concurrent_pixels[0] = result.context->readback_clear_color_pixel(); });
    std::thread second_readback(
        [&] { concurrent_pixels[1] = result.context->readback_clear_color_pixel(); });
    first_readback.join();
    second_readback.join();
    if (!concurrent_pixels[0] || !concurrent_pixels[1] || concurrent_pixels[0].rgba != pixel.rgba ||
        concurrent_pixels[1].rgba != pixel.rgba) {
        std::cerr << "FAIL: concurrent Vulkan queue submissions were not serialized correctly\n";
        return 1;
    }
    std::cout << "Vulkan offscreen clear/readback passed on " << device.name << '\n';
    return 0;
}
