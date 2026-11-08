#include "gpu/vulkan/vulkan_context.hpp"
#include "gpu/xenos/pm4_draw.hpp"
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

    const auto encode_vfetch = [](std::uint8_t destination, std::uint32_t offset_dwords) {
        return std::array<std::uint32_t, 3>{
            (static_cast<std::uint32_t>(destination) << 12U) | (1U << 19U) | (2U << 20U) |
                (1U << 25U),
            0x00000688U | (1U << 12U) | (1U << 13U) | (38U << 16U),
            (offset_dwords << 8U) | 8U,
        };
    };
    const std::array<std::array<std::uint32_t, 3>, 6> vertex_ucode{
        encode_vfetch(1U, 0U),
        encode_vfetch(3U, 4U),
        std::array<std::uint32_t, 3>{(0x0fU << 16U) | 2U, 0U,
                                    (1U << 31U) | (1U << 30U) | (4U << 8U) | 7U},
        std::array<std::uint32_t, 3>{62U | (1U << 15U) | (0x0fU << 16U), 0U,
                                    0xe0030201U},
        std::array<std::uint32_t, 3>{(1U << 15U) | (0x0fU << 16U), 0U,
                                    0xe0030203U},
        encode_vfetch(5U, 0U), // Slot 8 is referenced only by a skipped conditional block.
    };
    const std::array<ShaderExecInstruction, 3> vertex_exec_blocks{
        decode_shader_exec_instruction(
            {5U | (1U << 12U) | (0b01U << 16U),
             (3U << 12U) | (1U << 11U) | (1U << 8U)}),
        decode_shader_exec_instruction(
            {(0b11U | (0b11U << 2U) | (0b10U << 4U)) << 16U | (3U << 12U),
             1U << 12U}),
        decode_shader_exec_instruction(
            {3U | ((0b10U | (0b10U << 2U)) << 16U) | (2U << 12U), 2U << 12U}),
    };
    constexpr std::array<bool, 1> shader_boolean_constants{false};

    const auto append_pm4_u32 = [](std::vector<std::uint8_t>& bytes, std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
        bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(value));
    };
    std::vector<std::uint8_t> pm4_draw_stream;
    append_pm4_u32(pm4_draw_stream, (1U << 16U) | 0x200eU); // Screen scissor TL/BR.
    append_pm4_u32(pm4_draw_stream, 0U);
    append_pm4_u32(pm4_draw_stream, 16U | (16U << 16U));
    append_pm4_u32(pm4_draw_stream, (1U << 16U) | 0x2081U); // Window scissor TL/BR.
    append_pm4_u32(pm4_draw_stream, 1U << 31U); // Disable window offset.
    append_pm4_u32(pm4_draw_stream, 8U | (16U << 16U));
    append_pm4_u32(pm4_draw_stream, (2U << 16U) | 0x2100U); // VGT_MAX/MIN/INDX_OFFSET.
    append_pm4_u32(pm4_draw_stream, 0x00ffffffU);
    append_pm4_u32(pm4_draw_stream, 0U);
    append_pm4_u32(pm4_draw_stream, 0U);
    append_pm4_u32(pm4_draw_stream, 0xc0000000U | (4U << 16U) | (0x2dU << 8U));
    append_pm4_u32(pm4_draw_stream, 28U); // ALU float4 constant c7 (dword offset 7*4).
    append_pm4_u32(pm4_draw_stream, 0U);
    append_pm4_u32(pm4_draw_stream, 0U);
    append_pm4_u32(pm4_draw_stream, 0U);
    append_pm4_u32(pm4_draw_stream, 0U);
    append_pm4_u32(pm4_draw_stream, 0xc0000000U | (2U << 16U) | (0x2dU << 8U));
    append_pm4_u32(pm4_draw_stream, (1U << 16U) | 14U); // Fetch slot 7.
    append_pm4_u32(pm4_draw_stream, (0x80U << 2U) | 3U); // Guest base 0x200.
    append_pm4_u32(pm4_draw_stream, 24U << 2U); // 24 vertex dwords.
    append_pm4_u32(pm4_draw_stream, 0xc0000000U | (3U << 16U) | (0x22U << 8U));
    append_pm4_u32(pm4_draw_stream, 0U); // Visibility-query token.
    append_pm4_u32(pm4_draw_stream, (3U << 16U) | 4U); // Triangle list, DMA, 3 indexes.
    append_pm4_u32(pm4_draw_stream, 0x100U);
    append_pm4_u32(pm4_draw_stream, 3U);
    std::vector<std::uint32_t> draw_registers(0x5000U, 0U);
    const auto draw_dispatch = process_pm4_draw_stream(
        pm4_draw_stream, draw_registers, shader_memory);
    if (!draw_dispatch || draw_dispatch.draws.size() != 1U) {
        std::cerr << "FAIL: PM4 stream did not prepare its DMA draw"
                  << (draw_dispatch ? "" : ": " + draw_dispatch.error) << '\n';
        return 1;
    }
    const auto& prepared_geometry = draw_dispatch.draws[0].geometry;
    const auto scissor = read_xenos_scissor_rectangle(
        draw_dispatch.draws[0].register_snapshot, 16U, 16U);
    if (!scissor || scissor.rectangle != XenosScissorRectangle{0U, 0U, 8U, 16U}) {
        std::cerr << "FAIL: PM4 draw did not resolve its Xenos scissor state"
                  << (scissor ? "" : ": " + scissor.error) << '\n';
        return 1;
    }
    constexpr std::array<std::uint8_t, 1> required_fetch_slots{7U};
    const auto fetch_bank = decode_vertex_fetch_descriptor_bank(
        draw_dispatch.draws[0].register_snapshot, required_fetch_slots);
    if (!fetch_bank) {
        std::cerr << "FAIL: PM4 fetch constants did not decode: " << fetch_bank.error << '\n';
        return 1;
    }
    const auto& fetch_constants = fetch_bank.descriptors;
    constexpr std::array<std::uint8_t, 1> required_alu_constants{7U};
    const auto alu_constant_bank = decode_shader_alu_float_constant_bank(
        draw_dispatch.draws[0].register_snapshot, required_alu_constants);
    if (!alu_constant_bank) {
        std::cerr << "FAIL: PM4 ALU constants did not decode: " << alu_constant_bank.error
                  << '\n';
        return 1;
    }
    const auto vertex_batch = execute_pm4_draw_vertex_batch(
        draw_dispatch.draws[0], vertex_exec_blocks, vertex_ucode, shader_memory, {}, {},
        shader_boolean_constants);
    if (!vertex_batch) {
        std::cerr << "FAIL: synthetic Xenos vertex batch failed: " << vertex_batch.error << '\n';
        return 1;
    }
    auto missing_vertex_descriptor_draw = draw_dispatch.draws[0];
    missing_vertex_descriptor_draw.register_snapshot[0x480eU] = 0U;
    missing_vertex_descriptor_draw.register_snapshot[0x480fU] = 0U;
    const auto missing_vertex_descriptor_batch = execute_pm4_draw_vertex_batch(
        missing_vertex_descriptor_draw, vertex_exec_blocks, vertex_ucode, shader_memory,
        {}, {}, shader_boolean_constants);
    if (missing_vertex_descriptor_batch || !missing_vertex_descriptor_batch.vertices.empty() ||
        missing_vertex_descriptor_batch.error.find("VFETCH resources") == std::string::npos) {
        std::cerr << "FAIL: PM4 draw executor accepted a missing referenced VFETCH descriptor\n";
        return 1;
    }
    constexpr std::array<std::uint32_t, 3> out_of_range_vertex_indices{0U, 1U, 3U};
    const auto failed_vertex_batch = execute_shader_exec_vertex_batch(
        vertex_exec_blocks, vertex_ucode, fetch_constants, shader_memory,
        out_of_range_vertex_indices, {}, {}, alu_constant_bank.constants,
        shader_boolean_constants);
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
        vertex_batch.vertices, 0U, prepared_geometry, scissor.rectangle);
    if (!xenos_triangle ||
        xenos_triangle.center_rgba != std::array<std::uint8_t, 4>{255U, 0U, 255U, 255U} ||
        xenos_triangle.corner_rgba != std::array<std::uint8_t, 4>{255U, 0U, 255U, 255U} ||
        xenos_triangle.green_pixel_count == 0U ||
        xenos_triangle.green_pixel_count >= triangle.green_pixel_count) {
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
    std::cout << "Vulkan offscreen clear/readback, indexed triangle, PM4 scissor and Xenos EXEC output passed on "
              << device.name << " (" << triangle.green_pixel_count << " green pixels)\n";
    return 0;
}
