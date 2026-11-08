#include "gpu/xenos/pm4_decoder.hpp"
#include "gpu/xenos/pm4_registers.hpp"

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
    append_u32_be(draw_stream, 0xc0000000U | (0x22U << 8U)); // DRAW_INDX.
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
    test_wait_for_idle_is_synchronous_barrier();
    if (failures != 0) {
        std::cerr << failures << " GPU packet test(s) failed\n";
        return 1;
    }
    std::cout << "GPU packet decoder tests passed\n";
    return 0;
}
