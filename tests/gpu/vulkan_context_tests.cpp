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
    constexpr std::array<project360::gpu::vulkan::VulkanTestVertex, 3> vertices{{
        {{{-0.8F, -0.8F}}, {{0.0F, 1.0F, 0.0F, 1.0F}}},
        {{{0.8F, -0.8F}}, {{0.0F, 1.0F, 0.0F, 1.0F}}},
        {{{0.0F, 0.8F}}, {{0.0F, 1.0F, 0.0F, 1.0F}}},
    }};
    constexpr std::array<std::uint32_t, 3> indices{0U, 1U, 2U};
    const auto triangle = result.context->render_offscreen_indexed_triangles(vertices, indices);
    if (!triangle) {
        std::cerr << "FAIL: " << triangle.error << '\n';
        return 1;
    }
    if (triangle.center_rgba != std::array<std::uint8_t, 4>{0U, 255U, 0U, 255U} ||
        triangle.corner_rgba != std::array<std::uint8_t, 4>{255U, 0U, 255U, 255U} ||
        triangle.green_pixel_count == 0U || triangle.green_pixel_count >= 16U * 16U) {
        std::cerr << "FAIL: Vulkan triangle pixels do not match the expected rasterized output\n";
        return 1;
    }
    constexpr std::array<std::uint32_t, 3> invalid_indices{0U, 1U, 3U};
    const auto invalid_draw =
        result.context->render_offscreen_indexed_triangles(vertices, invalid_indices);
    if (invalid_draw || invalid_draw.error.find("missing vertex") == std::string::npos) {
        std::cerr << "FAIL: Vulkan indexed draw accepted an out-of-range vertex index\n";
        return 1;
    }
    auto blue_vertices = vertices;
    for (auto& vertex : blue_vertices) {
        vertex.color = {0.0F, 0.0F, 1.0F, 1.0F};
    }
    const auto blue_triangle =
        result.context->render_offscreen_indexed_triangles(blue_vertices, indices);
    if (!blue_triangle ||
        blue_triangle.center_rgba != std::array<std::uint8_t, 4>{0U, 0U, 255U, 255U} ||
        blue_triangle.corner_rgba != std::array<std::uint8_t, 4>{255U, 0U, 255U, 255U} ||
        blue_triangle.green_pixel_count != 0U) {
        std::cerr << "FAIL: Vulkan draw did not consume the uploaded vertex colors\n";
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
    std::cout << "Vulkan offscreen clear/readback and triangle passed on " << device.name
              << " (" << triangle.green_pixel_count << " green pixels)\n";
    return 0;
}
