#include "gpu/vulkan/vulkan_context.hpp"
#include "gpu/xenos/registers.hpp"
#include "gpu/xenos/shader_execution.hpp"
#include "memory/guest_memory.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
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
    const std::array<std::byte, 6> index_data{
        std::byte{0U}, std::byte{0U}, std::byte{1U},
        std::byte{0U}, std::byte{2U}, std::byte{0U}};
    constexpr std::array<float, 24> vertex_attributes{
        -1.6F, -1.6F, 0.0F, 2.0F, 0.0F, 1.0F, 0.0F, 1.0F,
        0.8F,  -0.8F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F,
        0.0F,  1.6F,  0.0F, 2.0F, 0.0F, 1.0F, 0.0F, 1.0F,
    };
    std::array<std::byte, vertex_attributes.size() * sizeof(float)> vertex_data{};
    for (std::size_t component = 0U; component < vertex_attributes.size(); ++component) {
        const auto bits = std::bit_cast<std::uint32_t>(vertex_attributes[component]);
        for (std::size_t byte = 0U; byte < sizeof(float); ++byte) {
            const auto shift = static_cast<std::uint32_t>(byte * 8U);
            vertex_data[component * sizeof(float) + byte] =
                static_cast<std::byte>(static_cast<std::uint8_t>((bits >> shift) & 0xffU));
        }
    }
    if (!shader_memory.load(0x200U, vertex_data) ||
        !shader_memory.load(0x100U, index_data)) {
        std::cerr << "FAIL: could not seed synthetic guest vertex/index buffers\n";
        return 1;
    }

    const auto fetch_descriptor =
        decode_vertex_fetch_descriptor((0x80U << 2U) | 3U, 24U << 2U);
    if (!fetch_descriptor) {
        std::cerr << "FAIL: synthetic VFETCH descriptor is invalid: " << fetch_descriptor.error
                  << '\n';
        return 1;
    }
    std::array<VertexFetchDescriptor, 8> fetch_constants{};
    fetch_constants[7] = fetch_descriptor.descriptor;
    const auto encode_vfetch = [](std::uint8_t destination, std::uint32_t offset_dwords) {
        return std::array<std::uint32_t, 3>{
            (static_cast<std::uint32_t>(destination) << 12U) | (1U << 19U) | (2U << 20U) |
                (1U << 25U),
            0x00000688U | (1U << 12U) | (1U << 13U) | (38U << 16U),
            (offset_dwords << 8U) | 8U,
        };
    };
    const std::array<std::array<std::uint32_t, 3>, 4> vertex_ucode{
        encode_vfetch(1U, 0U),
        encode_vfetch(3U, 4U),
        std::array<std::uint32_t, 3>{62U | (1U << 15U) | (0x0fU << 16U), 0U,
                                    0xe0030201U},
        std::array<std::uint32_t, 3>{(1U << 15U) | (0x0fU << 16U), 0U,
                                    0xe0030203U},
    };
    const std::array<ShaderExecInstruction, 2> vertex_exec_blocks{
        decode_shader_exec_instruction(
            {(0b11U | (0b11U << 2U)) << 16U | (2U << 12U), 1U << 12U}),
        decode_shader_exec_instruction(
            {2U | ((0b10U | (0b10U << 2U)) << 16U) | (2U << 12U), 2U << 12U}),
    };

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
    const auto vertex_batch = execute_shader_exec_vertex_batch(
        vertex_exec_blocks, vertex_ucode, fetch_constants, shader_memory,
        prepared_geometry.indices, {}, {});
    if (!vertex_batch) {
        std::cerr << "FAIL: synthetic Xenos vertex batch failed: " << vertex_batch.error << '\n';
        return 1;
    }
    constexpr std::array<std::uint32_t, 3> out_of_range_vertex_indices{0U, 1U, 3U};
    const auto failed_vertex_batch = execute_shader_exec_vertex_batch(
        vertex_exec_blocks, vertex_ucode, fetch_constants, shader_memory,
        out_of_range_vertex_indices, {}, {});
    if (failed_vertex_batch || !failed_vertex_batch.vertices.empty() ||
        failed_vertex_batch.error.find("item 2 (index 3)") == std::string::npos) {
        std::cerr << "FAIL: Xenos vertex batch exposed partial outputs after VFETCH failure\n";
        return 1;
    }
    constexpr std::array<std::uint32_t, 1> invalid_vertex_index{0x01000000U};
    const auto rejected_vertex_index = execute_shader_exec_vertex_batch(
        vertex_exec_blocks, vertex_ucode, fetch_constants, shader_memory,
        invalid_vertex_index, {}, {});
    if (rejected_vertex_index || !rejected_vertex_index.vertices.empty()) {
        std::cerr << "FAIL: Xenos vertex batch accepted an index wider than 24 bits\n";
        return 1;
    }
    const auto xenos_triangle = result.context->render_offscreen_xenos_geometry(
        vertex_batch.vertices, 0U, prepared_geometry);
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
        vertex_batch.vertices, 0U, malformed_geometry);
    if (malformed_draw || malformed_draw.error.find("invalid or incomplete ranges") ==
                              std::string::npos) {
        std::cerr << "FAIL: Vulkan accepted malformed prepared Xenos triangle ranges\n";
        return 1;
    }
    auto line_geometry = prepared_geometry;
    line_geometry.topology = project360::gpu::xenos::PrimitiveTopology::lines;
    const auto unsupported_topology = result.context->render_offscreen_xenos_geometry(
        vertex_batch.vertices, 0U, line_geometry);
    if (unsupported_topology ||
        unsupported_topology.error.find("triangle lists only") == std::string::npos) {
        std::cerr << "FAIL: Vulkan diagnostic accepted a non-triangle Xenos topology\n";
        return 1;
    }
    auto incomplete_xenos_vertex = vertex_batch.vertices;
    incomplete_xenos_vertex[0].position_write_mask = 0x07U;
    const auto incomplete_xenos_draw =
        result.context->render_offscreen_xenos_triangles(incomplete_xenos_vertex, 0U, indices);
    if (incomplete_xenos_draw ||
        incomplete_xenos_draw.error.find("missing position") == std::string::npos) {
        std::cerr << "FAIL: Xenos draw adapter accepted an incomplete vertex export\n";
        return 1;
    }
    const auto invalid_color_export =
        result.context->render_offscreen_xenos_geometry(vertex_batch.vertices, 16U,
                                                        prepared_geometry);
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
