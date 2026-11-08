#include "gpu/xenos/pm4_decoder.hpp"
#include "gpu/xenos/pm4_draw.hpp"
#include "gpu/xenos/pm4_registers.hpp"
#include "memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void append_u32_be(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void test_packet_framing_and_metadata() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> stream;
    append_u32_be(stream, (1U << 16U) | 0x0123U); // Type 0, two sequential registers.
    append_u32_be(stream, 0x11111111U);
    append_u32_be(stream, 0x22222222U);
    append_u32_be(stream, 0x40000000U | (0x345U << 11U) | 0x123U); // Type 1.
    append_u32_be(stream, 0x33333333U);
    append_u32_be(stream, 0x44444444U);
    append_u32_be(stream, 0x80000000U); // Type 2 no-op.
    append_u32_be(stream, 0xc0000000U | (2U << 16U) | (0x2dU << 8U) | 1U); // Type 3.
    append_u32_be(stream, 0x55555555U);
    append_u32_be(stream, 0x66666666U);
    append_u32_be(stream, 0x77777777U);

    const auto decoded = decode_pm4(stream);
    check(static_cast<bool>(decoded) && decoded.packets.size() == 4U,
          "mixed PM4 stream decodes into four packets");
    if (!decoded || decoded.packets.size() != 4U) return;

    const auto& type0 = decoded.packets[0];
    check(type0.type == PacketType::type0_registers && type0.register_index == 0x123U &&
              type0.payload_word_count == 2U && type0.payload_word_offset == 1U &&
              !type0.write_one_register,
          "type-0 header exposes base register, sequential mode and bounded payload");
    const auto& type1 = decoded.packets[1];
    check(type1.type == PacketType::type1_registers && type1.register_index == 0x123U &&
              type1.second_register_index == 0x345U && type1.payload_word_count == 2U,
          "type-1 header exposes both register indexes and its two-word payload");
    check(decoded.packets[2].type == PacketType::type2_nop &&
              decoded.packets[2].payload_word_count == 0U,
          "type-2 packet consumes only its header");
    const auto& type3 = decoded.packets[3];
    check(type3.type == PacketType::type3_command && type3.opcode == 0x2dU &&
              type3.predicated && type3.payload_word_count == 3U &&
              type3.payload_word_offset == 8U,
          "type-3 header exposes opcode, predicate bit and payload bounds");
}

void test_noops_and_malformed_streams() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> noops;
    append_u32_be(noops, 0U);
    append_u32_be(noops, 0x80000000U);
    const auto decoded_noops = decode_pm4(noops);
    check(static_cast<bool>(decoded_noops) && decoded_noops.packets.size() == 2U &&
              decoded_noops.packets[0].type == PacketType::empty_nop &&
              decoded_noops.packets[1].type == PacketType::type2_nop,
          "zero header and type-2 packet are decoded as no-ops");

    std::vector<std::uint8_t> truncated_type0;
    append_u32_be(truncated_type0, (2U << 16U) | 0x20U); // Three payload words.
    append_u32_be(truncated_type0, 0xabcdef01U);
    const auto type0_error = decode_pm4(truncated_type0);
    check(!type0_error && type0_error.packets.empty() && type0_error.error_word_offset == 0U,
          "truncated type-0 packet fails without exposing partial decode results");

    std::vector<std::uint8_t> truncated_type1;
    append_u32_be(truncated_type1, 0x40000000U);
    append_u32_be(truncated_type1, 0U);
    const auto type1_error = decode_pm4(truncated_type1);
    check(!type1_error && type1_error.packets.empty(),
          "type-1 packet requires both data dwords");

    std::vector<std::uint8_t> truncated_type3;
    append_u32_be(truncated_type3, 0xc0000000U | (0x3fffU << 16U) | (7U << 8U));
    const auto type3_error = decode_pm4(truncated_type3);
    check(!type3_error && type3_error.packets.empty(),
          "maximum declared type-3 payload is rejected when truncated");

    std::vector<std::uint8_t> partial_dword{0U, 1U, 2U};
    const auto alignment_error = decode_pm4(partial_dword);
    check(!alignment_error && alignment_error.packets.empty(),
          "stream with a partial dword is rejected transactionally");
}

void test_register_packet_application() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> stream;
    append_u32_be(stream, (1U << 16U) | 0x0123U);
    append_u32_be(stream, 0x11111111U);
    append_u32_be(stream, 0x22222222U);
    append_u32_be(stream, 0x40000000U | (0x345U << 11U) | 0x123U);
    append_u32_be(stream, 0x33333333U);
    append_u32_be(stream, 0x44444444U);
    append_u32_be(stream, 0x80000000U);
    append_u32_be(stream, 0xc0000000U | (0x10U << 8U));
    append_u32_be(stream, 0x55555555U);

    const auto decoded = decode_pm4(stream);
    std::vector<std::uint32_t> registers(0x400U, 0U);
    if (decoded) {
        const auto applied = apply_register_packets(decoded.packets, stream, registers);
        check(static_cast<bool>(applied), "valid type-0/type-1 register writes apply");
        check(registers[0x123U] == 0x33333333U && registers[0x124U] == 0x22222222U &&
                  registers[0x345U] == 0x44444444U,
              "sequential type-0 and indexed type-1 writes update addressed registers");
        check(registers[0U] == 0U,
              "type-2 no-op and type-3 command payload do not alter register state");
    } else {
        check(false, "register test stream parses before applying writes");
    }

    std::vector<std::uint8_t> repeated_stream;
    append_u32_be(repeated_stream, (1U << 16U) | (1U << 15U) | 0x10U);
    append_u32_be(repeated_stream, 0xaaaa0001U);
    append_u32_be(repeated_stream, 0xaaaa0002U);
    const auto repeated_decoded = decode_pm4(repeated_stream);
    std::vector<std::uint32_t> repeated_registers(0x20U, 0x55555555U);
    const auto repeated_result = apply_register_packets(repeated_decoded.packets,
                                                        repeated_stream, repeated_registers);
    check(static_cast<bool>(repeated_result) && repeated_registers[0x10U] == 0xaaaa0002U &&
              repeated_registers[0x11U] == 0x55555555U,
          "type-0 write-one mode repeatedly targets the same register");

    std::vector<std::uint8_t> invalid_stream;
    append_u32_be(invalid_stream, 2U);
    append_u32_be(invalid_stream, 0xdeadbeefU);
    append_u32_be(invalid_stream, 0x40000000U | (4U << 11U) | 1U);
    append_u32_be(invalid_stream, 0x11111111U);
    append_u32_be(invalid_stream, 0x22222222U);
    const auto invalid_decoded = decode_pm4(invalid_stream);
    std::vector<std::uint32_t> protected_registers(4U, 0xabcdef01U);
    const auto invalid_result = apply_register_packets(invalid_decoded.packets,
                                                       invalid_stream, protected_registers);
    check(!invalid_result && protected_registers[0] == 0xabcdef01U &&
              protected_registers[1] == 0xabcdef01U &&
              protected_registers[2] == 0xabcdef01U &&
              protected_registers[3] == 0xabcdef01U,
          "out-of-range later write rejects the complete stream without partial register changes");

    auto stale_stream = stream;
    stale_stream[0] ^= 0x01U;
    std::vector<std::uint32_t> stale_registers(0x400U, 0xabcdef01U);
    const auto stale_result = apply_register_packets(decoded.packets, stale_stream,
                                                     stale_registers);
    check(!stale_result && stale_registers[0x123U] == 0xabcdef01U,
          "register application rejects decoded packet metadata from a different stream");
}

void test_type3_constant_uploads() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> stream;
    const auto add_set_constant = [&](std::uint32_t type, std::uint32_t index,
                                      std::uint32_t value) {
        append_u32_be(stream, 0xc0000000U | (1U << 16U) | (0x2dU << 8U));
        append_u32_be(stream, (type << 16U) | index);
        append_u32_be(stream, value);
    };
    add_set_constant(0U, 3U, 0xa0000000U);
    add_set_constant(1U, 5U, 0xa1000000U);
    add_set_constant(2U, 6U, 0xa2000000U);
    add_set_constant(3U, 1U, 0xa3000000U);
    add_set_constant(4U, 7U, 0xa4000000U);
    append_u32_be(stream, 0xc0000000U | (2U << 16U) | (0x55U << 8U));
    append_u32_be(stream, 0x3456U);
    append_u32_be(stream, 0xb0000000U);
    append_u32_be(stream, 0xb0000001U);
    append_u32_be(stream, 0xc0000000U | (1U << 16U) | (0x56U << 8U));
    append_u32_be(stream, 0x4321U);
    append_u32_be(stream, 0xc0000000U);

    const auto decoded = decode_pm4(stream);
    std::vector<std::uint32_t> registers(0x5000U, 0U);
    if (!decoded) {
        check(false, "constant upload test stream parses");
        return;
    }
    const auto applied = apply_register_packets(decoded.packets, stream, registers);
    check(static_cast<bool>(applied), "supported type-3 constant uploads apply");
    check(registers[0x4003U] == 0xa0000000U && registers[0x4805U] == 0xa1000000U &&
              registers[0x4906U] == 0xa2000000U && registers[0x4909U] == 0xa3000000U &&
              registers[0x2007U] == 0xa4000000U,
          "SET_CONSTANT maps ALU, fetch, boolean, loop and direct register banks");
    check(registers[0x3456U] == 0xb0000000U && registers[0x3457U] == 0xb0000001U &&
              registers[0x4321U] == 0xc0000000U,
          "SET_CONSTANT2 and SET_SHADER_CONSTANTS write direct register indexes");

    std::vector<std::uint8_t> invalid_stream;
    append_u32_be(invalid_stream, 0x00000004U);
    append_u32_be(invalid_stream, 0x11111111U);
    append_u32_be(invalid_stream, 0xc0000000U | (1U << 16U) | (0x2dU << 8U));
    append_u32_be(invalid_stream, (6U << 16U) | 2U); // Unsupported constant bank.
    append_u32_be(invalid_stream, 0x22222222U);
    const auto invalid_decoded = decode_pm4(invalid_stream);
    std::vector<std::uint32_t> unchanged(0x5000U, 0xccccccccU);
    const auto invalid_result = apply_register_packets(invalid_decoded.packets,
                                                       invalid_stream, unchanged);
    check(!invalid_result && unchanged[4U] == 0xccccccccU,
          "unsupported SET_CONSTANT bank rejects prior writes transactionally");
}

void test_unimplemented_commands_fail_explicitly() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> draw_stream;
    append_u32_be(draw_stream, 4U);
    append_u32_be(draw_stream, 0x12345678U);
    append_u32_be(draw_stream, 0xc0000000U | (1U << 16U) | (0x22U << 8U)); // DRAW_INDX.
    append_u32_be(draw_stream, 0U); // Visibility query condition.
    append_u32_be(draw_stream, (3U << 16U) | (2U << 6U)); // Auto-index, triangle list.
    const auto draw_decoded = decode_pm4(draw_stream);
    std::vector<std::uint32_t> draw_registers(8U, 0xccccccccU);
    const auto draw_result = apply_register_packets(draw_decoded.packets, draw_stream,
                                                   draw_registers);
    check(!draw_result && draw_registers[4U] == 0xccccccccU,
          "unimplemented draw command is reported without committing preceding writes");

    std::vector<std::uint8_t> predicated_stream;
    append_u32_be(predicated_stream, 0xc0000000U | (1U << 16U) |
                                       (0x2dU << 8U) | 1U);
    append_u32_be(predicated_stream, 0U);
    append_u32_be(predicated_stream, 0xabcdef01U);
    const auto predicated_decoded = decode_pm4(predicated_stream);
    std::vector<std::uint32_t> predicated_registers(0x5000U, 0xccccccccU);
    const auto predicated_result = apply_register_packets(
        predicated_decoded.packets, predicated_stream, predicated_registers);
    check(!predicated_result && predicated_registers[0x4000U] == 0xccccccccU,
          "predicated constant upload fails until bin-selection state is modeled");

    std::vector<std::uint8_t> complete_stream;
    append_u32_be(complete_stream, 0x80000000U);
    const auto complete_decoded = decode_pm4(complete_stream);
    std::vector<std::uint32_t> complete_registers(1U, 0U);
    const auto incomplete_list_result = apply_register_packets(
        std::span<const Packet>(complete_decoded.packets.data(), 0U), complete_stream,
        complete_registers);
    check(!incomplete_list_result,
          "register application rejects a packet list that omits stream contents");
}

void test_draw_packet_metadata_decoding() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> stream;
    // DRAW_INDX: query token, 16-bit DMA indices, triangle list, 3 indices.
    append_u32_be(stream, 0xc0000000U | (3U << 16U) | (0x22U << 8U));
    append_u32_be(stream, 0x00000105U);
    append_u32_be(stream, (3U << 16U) | 3U);
    append_u32_be(stream, 0x00123456U);
    append_u32_be(stream, 0x00000003U);
    // DRAW_INDX_2: auto-index point list, two indices, with no query token.
    append_u32_be(stream, 0xc0000000U | (0x36U << 8U));
    append_u32_be(stream, (2U << 16U) | (2U << 6U) | 2U);
    const auto packets = decode_pm4(stream);
    const auto draws = packets ? decode_draw_commands(packets.packets, stream)
                               : DrawDecodeResult{{}, "PM4 decode failed", 0U};
    check(static_cast<bool>(draws) && draws.commands.size() == 2U,
          "DRAW_INDX and DRAW_INDX_2 produce metadata without submitting work");
    if (draws && draws.commands.size() == 2U) {
        const auto& dma = draws.commands[0];
        check(dma.has_viz_query_condition && dma.viz_query_condition == 0x105U &&
                  dma.source == DrawSource::dma && dma.primitive_type == 3U &&
                  dma.index_count == 3U && !dma.index_32_bit &&
                  dma.dma_base == 0x00123456U && dma.dma_size == 3U,
              "DMA draw exposes query token, initiator fields, base and size");
        const auto& automatic = draws.commands[1];
        check(!automatic.has_viz_query_condition && automatic.source == DrawSource::auto_index &&
                  automatic.primitive_type == 2U && automatic.index_count == 2U,
              "DRAW_INDX_2 decodes auto-index metadata without a query prefix");

        std::vector<std::uint32_t> registers(0x2200U, 0U);
        const auto applied = apply_draw_command_state(draws.commands, registers);
        check(static_cast<bool>(applied) && registers[0x21fcU] == automatic.initiator &&
                  registers[0x21faU] == 0x00123456U && registers[0x21fbU] == 3U,
              "draw state updates initiator and DMA registers while preserving DMA state "
              "for auto-index");

        std::vector<std::uint32_t> too_small(0x21fcU, 0xccccccccU);
        const auto rejected = apply_draw_command_state(draws.commands, too_small);
        check(!rejected && too_small.back() == 0xccccccccU,
              "out-of-range draw state rejects the command list without partial writes");

        auto forged = draws.commands;
        forged[1].index_count++;
        std::fill(registers.begin(), registers.end(), 0xccccccccU);
        const auto inconsistent = apply_draw_command_state(forged, registers);
        check(!inconsistent && registers[0x21fcU] == 0xccccccccU,
              "inconsistent decoded draw metadata fails before changing register state");
    }

    std::vector<std::uint8_t> truncated;
    append_u32_be(truncated, 0xc0000000U | (0x22U << 8U));
    append_u32_be(truncated, 0U); // Query token, but missing initiator.
    const auto truncated_packets = decode_pm4(truncated);
    const auto missing_initiator = decode_draw_commands(truncated_packets.packets, truncated);
    check(!missing_initiator && missing_initiator.commands.empty(),
          "draw packet missing initiator fails without partial commands");

    std::vector<std::uint8_t> immediate;
    append_u32_be(immediate, 0xc0000000U | (2U << 16U) | (0x36U << 8U));
    append_u32_be(immediate, (1U << 6U) | 3U);
    append_u32_be(immediate, 0x1234U);
    const auto immediate_packets = decode_pm4(immediate);
    const auto unsupported = decode_draw_commands(immediate_packets.packets, immediate);
    check(!unsupported && unsupported.commands.empty(),
          "immediate-index draw reports unsupported semantics explicitly");
}

void test_dma_index_fetch() {
    using namespace project360::gpu::xenos;
    project360::memory::GuestMemory memory(0x4000U);
    const std::array<std::byte, 4> index16_bytes{
        std::byte{0x12}, std::byte{0x34}, std::byte{0xab}, std::byte{0xcd}};
    check(memory.load(8U, index16_bytes), "16-bit DMA fixture loads into physical memory");
    check(memory.map_page(0x1000U, 0U, project360::memory::GuestPageAccess::read),
          "CPU virtual mapping can coexist with GPU physical reads");

    DrawCommand draw16{};
    draw16.source = DrawSource::dma;
    draw16.index_count = 2U;
    draw16.dma_base = 8U;
    draw16.dma_size = (1U << 30U) | 2U; // 8-in-16, two index elements.
    const auto swapped16 = fetch_draw_indices(draw16, memory);
    check(static_cast<bool>(swapped16) && swapped16.indices.size() == 2U &&
              swapped16.indices[0] == 0x1234U && swapped16.indices[1] == 0xabcdU,
          "16-bit DMA fetch applies the supported 8-in-16 byte swap");

    draw16.dma_size = 2U; // No swap.
    const auto native16 = fetch_draw_indices(draw16, memory);
    check(static_cast<bool>(native16) && native16.indices[0] == 0x3412U &&
              native16.indices[1] == 0xcdabU,
          "16-bit DMA fetch preserves bytes in no-swap mode");

    const std::array<std::byte, 4> index32_bytes{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    check(memory.load(16U, index32_bytes), "32-bit DMA fixture loads into physical memory");
    constexpr std::array<std::uint32_t, 4> expected_by_mode{
        0x44332211U, 0x33441122U, 0x11223344U, 0x22114433U};
    DrawCommand draw32{};
    draw32.source = DrawSource::dma;
    draw32.index_32_bit = true;
    draw32.index_count = 1U;
    draw32.dma_base = 16U;
    for (std::uint32_t mode = 0U; mode < 4U; ++mode) {
        draw32.dma_size = (mode << 30U) | 1U;
        const auto fetched = fetch_draw_indices(draw32, memory);
        check(static_cast<bool>(fetched) && fetched.indices.size() == 1U &&
                  fetched.indices[0] == expected_by_mode[mode],
              "32-bit DMA fetch matches each documented byte-swap mode");
    }

    draw16.dma_size = (3U << 30U) | 2U;
    const auto ambiguous16 = fetch_draw_indices(draw16, memory);
    check(!ambiguous16 && ambiguous16.indices.empty(),
          "ambiguous 32-bit swap mode on 16-bit indexes is rejected explicitly");

    draw16.dma_size = 1U;
    const auto short_dma = fetch_draw_indices(draw16, memory);
    check(!short_dma && short_dma.indices.empty(),
          "DMA index count cannot exceed its declared element capacity");

    draw32.dma_base = 0x4000U;
    draw32.dma_size = 1U;
    const auto out_of_bounds = fetch_draw_indices(draw32, memory);
    check(!out_of_bounds && out_of_bounds.indices.empty(),
          "DMA index access outside physical guest memory fails without partial output");

    DrawCommand automatic{};
    automatic.source = DrawSource::auto_index;
    automatic.index_count = 4U;
    const auto generated = fetch_draw_indices(automatic, memory);
    check(static_cast<bool>(generated) && generated.indices.size() == 4U &&
              generated.indices[0] == 0U && generated.indices[3] == 3U,
          "auto-index source generates the guest sequential index range");
}

void test_primitive_assembly() {
    using namespace project360::gpu::xenos;
    DrawCommand command{};
    command.source = DrawSource::auto_index;
    command.primitive_type = 5U; // Triangle fan.
    command.index_count = 4U;
    const std::array<std::uint32_t, 4> fan{10U, 11U, 12U, 13U};
    const auto fan_result = assemble_draw_primitives(command, fan);
    check(static_cast<bool>(fan_result) &&
              fan_result.topology == PrimitiveTopology::triangles &&
              fan_result.indices == std::vector<std::uint32_t>{11U, 12U, 10U, 12U, 13U,
                                                               10U} &&
              fan_result.ranges.size() == 1U && fan_result.ranges[0].index_count == 6U,
          "triangle fan becomes indexed triangles with a bounded draw range");

    command.primitive_type = 0x0cU; // Line loop.
    const auto loop_result = assemble_draw_primitives(command, fan);
    check(static_cast<bool>(loop_result) && loop_result.topology == PrimitiveTopology::lines &&
              loop_result.indices == std::vector<std::uint32_t>{10U, 11U, 11U, 12U, 12U,
                                                                13U, 13U, 10U},
          "line loop closes each sequence as independent line segments");

    command.primitive_type = 0x0dU; // Quad list.
    command.index_count = 5U;
    const std::array<std::uint32_t, 5> quad{20U, 21U, 22U, 23U, 24U};
    const auto quad_result = assemble_draw_primitives(command, quad);
    check(static_cast<bool>(quad_result) &&
              quad_result.topology == PrimitiveTopology::triangles &&
              quad_result.indices == std::vector<std::uint32_t>{20U, 21U, 22U, 20U, 22U,
                                                                 23U},
          "quad list uses two triangles and ignores an incomplete trailing quad");

    command.primitive_type = 6U; // Triangle strip.
    command.index_32_bit = true;
    command.index_count = 7U;
    const std::array<std::uint32_t, 7> split_strip{1U, 2U, 3U, 0x120000ffU,
                                                   4U, 5U, 6U};
    const auto strip_result = assemble_draw_primitives(
        command, split_strip, {.restart_enabled = true, .restart_index = 0x000000ffU});
    check(static_cast<bool>(strip_result) &&
              strip_result.topology == PrimitiveTopology::triangle_strip &&
              strip_result.indices == std::vector<std::uint32_t>{1U, 2U, 3U, 4U, 5U, 6U} &&
              strip_result.ranges.size() == 2U && strip_result.ranges[0].index_count == 3U &&
              strip_result.ranges[1].first_index == 3U,
          "32-bit primitive restart splits ranges using the low 24-bit reset value");

    command.primitive_type = 5U;
    command.index_32_bit = false;
    command.index_count = 7U;
    const std::array<std::uint32_t, 7> split_fan{1U, 2U, 3U, 0xffU, 4U, 5U, 6U};
    const auto restart_fan = assemble_draw_primitives(
        command, split_fan, {.restart_enabled = true, .restart_index = 0xffU});
    check(static_cast<bool>(restart_fan) && restart_fan.ranges.size() == 2U &&
              restart_fan.indices == std::vector<std::uint32_t>{2U, 3U, 1U, 5U, 6U, 4U},
          "16-bit primitive restart splits triangle fans before fan expansion");

    command.primitive_type = 4U;
    const auto unsupported_restart = assemble_draw_primitives(
        command, split_fan, {.restart_enabled = true, .restart_index = 0xffU});
    check(!unsupported_restart && unsupported_restart.indices.empty(),
          "restart on a topology without modeled Xenos semantics fails explicitly");
    command.primitive_type = 0x10U;
    const auto unsupported_type = assemble_draw_primitives(command, split_fan);
    check(!unsupported_type && unsupported_type.indices.empty(),
          "unsupported Xenos topology is rejected without generated indices");
    command.primitive_type = 4U;
    command.index_count = 2U;
    const auto count_mismatch = assemble_draw_primitives(command, split_fan);
    check(!count_mismatch && count_mismatch.indices.empty(),
          "primitive assembly rejects an index span inconsistent with draw metadata");
}

void test_wait_for_idle_is_synchronous_barrier() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> stream;
    append_u32_be(stream, 0xc0000000U | (0x26U << 8U)); // WAIT_FOR_IDLE, one dword.
    append_u32_be(stream, 0U);
    const auto decoded = decode_pm4(stream);
    std::vector<std::uint32_t> registers(1U, 0x12345678U);
    const auto applied = apply_register_packets(decoded.packets, stream, registers);
    check(static_cast<bool>(applied) && registers[0] == 0x12345678U,
          "WAIT_FOR_IDLE completes immediately with the synchronous unimplemented GPU");

    auto malformed = stream;
    malformed[1U] = 1U; // Two payload dwords instead of the required one.
    append_u32_be(malformed, 0U);
    const auto malformed_decoded = decode_pm4(malformed);
    const auto rejected = apply_register_packets(malformed_decoded.packets, malformed,
                                                 registers);
    check(!rejected && registers[0] == 0x12345678U,
          "malformed WAIT_FOR_IDLE packet is rejected without changing registers");
}

} // namespace

int main() {
    test_packet_framing_and_metadata();
    test_noops_and_malformed_streams();
    test_register_packet_application();
    test_type3_constant_uploads();
    test_unimplemented_commands_fail_explicitly();
    test_draw_packet_metadata_decoding();
    test_dma_index_fetch();
    test_primitive_assembly();
    test_wait_for_idle_is_synchronous_barrier();
    if (failures != 0) {
        std::cerr << failures << " GPU packet test(s) failed\n";
        return 1;
    }
    std::cout << "GPU packet decoder tests passed\n";
    return 0;
}
