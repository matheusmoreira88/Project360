#include "gpu/vulkan/vulkan_context.hpp"
#include "gpu/xenos/registers.hpp"
#include "gpu/xenos/shader_execution.hpp"
#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>

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
        {{{-0.8F, -0.8F, 0.0F, 1.0F}}, {{0.0F, 1.0F, 0.0F, 1.0F}}},
        {{{0.8F, -0.8F, 0.0F, 1.0F}}, {{0.0F, 1.0F, 0.0F, 1.0F}}},
        {{{0.0F, 0.8F, 0.0F, 1.0F}}, {{0.0F, 1.0F, 0.0F, 1.0F}}},
    }};
    constexpr std::array<std::uint32_t, 3> indices{0U, 1U, 2U};
    const auto triangle = result.context->render_offscreen_indexed_triangles(vertices, indices);
    if (!triangle) {
        std::cerr << "FAIL: " << triangle.error << '\n';
        return 1;
    }

    using namespace project360::gpu::xenos;
    project360::memory::GuestMemory shader_memory(0x1000U);
    const std::array<std::array<std::uint32_t, 3>, 2> vertex_ucode{
        std::array<std::uint32_t, 3>{62U | (1U << 15U) | (0x0fU << 16U), 0U,
                                    0xe0030201U},
        std::array<std::uint32_t, 3>{(1U << 15U) | (0x0fU << 16U), 0U,
                                    0xe0030403U}};
    const auto vertex_exec = decode_shader_exec_instruction(
        {(0b1010U << 16U) | (2U << 12U), 2U << 12U});
    constexpr std::array<std::array<float, 4>, 3> clip_positions{{
        {{-1.6F, -1.6F, 0.0F, 2.0F}},
        {{0.8F, -0.8F, 0.0F, 1.0F}},
        {{0.0F, 1.6F, 0.0F, 2.0F}},
    }};
    constexpr std::array<float, 4> green{0.0F, 1.0F, 0.0F, 1.0F};
    std::array<ShaderVertexOutputs, 3> xenos_vertices{};
    for (std::size_t vertex_index = 0U; vertex_index < xenos_vertices.size(); ++vertex_index) {
        ShaderAluTemporaryRegisterBank registers{};
        registers[1] = clip_positions[vertex_index];
        registers[3] = green;
        const auto shader = execute_shader_exec_block(
            vertex_exec, vertex_ucode, {}, shader_memory, registers, {});
        if (!shader) {
            std::cerr << "FAIL: Xenos vertex EXEC did not complete: " << shader.error << '\n';
            return 1;
        }
        xenos_vertices[vertex_index] = shader.vertex_outputs;
    }
    const std::array<std::byte, 6> index_data{
        std::byte{0U}, std::byte{0U}, std::byte{1U},
        std::byte{0U}, std::byte{2U}, std::byte{0U}};
    if (!shader_memory.load(0x100U, index_data)) {
        std::cerr << "FAIL: could not seed synthetic guest index buffer\n";
        return 1;
    }
    project360::gpu::xenos::DrawCommand draw_command{};
    draw_command.source = project360::gpu::xenos::DrawSource::dma;
    draw_command.index_count = 3U;
    draw_command.primitive_type = 4U;
    draw_command.dma_base = 0x100U;
    draw_command.dma_size = 3U;
    std::vector<std::uint32_t> draw_registers(
        project360::gpu::xenos::register_index::vgt_indx_offset + 1U, 0U);
    draw_registers[project360::gpu::xenos::register_index::vgt_max_vtx_indx] = 0x00ffffffU;
    const auto prepared_geometry =
        project360::gpu::xenos::prepare_draw_geometry(draw_command, shader_memory,
                                                       draw_registers);
    const auto xenos_triangle = result.context->render_offscreen_xenos_geometry(
        xenos_vertices, 0U, prepared_geometry);
    if (!xenos_triangle ||
        xenos_triangle.center_rgba != std::array<std::uint8_t, 4>{0U, 255U, 0U, 255U} ||
        xenos_triangle.corner_rgba != std::array<std::uint8_t, 4>{255U, 0U, 255U, 255U} ||
        xenos_triangle.green_pixel_count != triangle.green_pixel_count) {
        std::cerr << "FAIL: CPU Xenos exports did not survive Vulkan vertex upload and rasterization"
                  << (xenos_triangle ? "" : ": " + xenos_triangle.error) << '\n';
        return 1;
    }
    auto malformed_geometry = prepared_geometry;
    malformed_geometry.ranges[0].index_count = 2U;
    const auto malformed_draw = result.context->render_offscreen_xenos_geometry(
        xenos_vertices, 0U, malformed_geometry);
    if (malformed_draw || malformed_draw.error.find("invalid or incomplete ranges") ==
                              std::string::npos) {
        std::cerr << "FAIL: Vulkan accepted malformed prepared Xenos triangle ranges\n";
        return 1;
    }
    auto line_geometry = prepared_geometry;
    line_geometry.topology = project360::gpu::xenos::PrimitiveTopology::lines;
    const auto unsupported_topology = result.context->render_offscreen_xenos_geometry(
        xenos_vertices, 0U, line_geometry);
    if (unsupported_topology ||
        unsupported_topology.error.find("triangle lists only") == std::string::npos) {
        std::cerr << "FAIL: Vulkan diagnostic accepted a non-triangle Xenos topology\n";
        return 1;
    }
    auto incomplete_xenos_vertex = xenos_vertices;
    incomplete_xenos_vertex[0].position_write_mask = 0x07U;
    const auto incomplete_xenos_draw =
        result.context->render_offscreen_xenos_triangles(incomplete_xenos_vertex, 0U, indices);
    if (incomplete_xenos_draw ||
        incomplete_xenos_draw.error.find("missing position") == std::string::npos) {
        std::cerr << "FAIL: Xenos draw adapter accepted an incomplete vertex export\n";
        return 1;
    }
    const auto invalid_color_export =
        result.context->render_offscreen_xenos_triangles(xenos_vertices, 16U, indices);
    if (invalid_color_export || invalid_color_export.error.find("interpolator 0..15") ==
                                    std::string::npos) {
        std::cerr << "FAIL: Xenos draw adapter accepted an out-of-range color interpolator\n";
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
    std::cout << "Vulkan offscreen clear/readback, indexed triangle and Xenos EXEC output passed on "
              << device.name << " (" << triangle.green_pixel_count << " green pixels)\n";
    return 0;
}
