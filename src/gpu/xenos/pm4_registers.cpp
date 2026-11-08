#include "gpu/xenos/pm4_registers.hpp"

#include <utility>
#include <vector>

namespace project360::gpu::xenos {
namespace {

struct RegisterWrite {
    std::size_t index{};
    std::uint32_t value{};
};

std::uint32_t read_u32_be(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

RegisterApplyResult failure(const char* message, std::size_t word_offset) {
    return {message, word_offset};
}

} // namespace

RegisterApplyResult apply_register_packets(std::span<const Packet> packets,
                                          std::span<const std::uint8_t> stream,
                                          std::span<std::uint32_t> registers) {
    if ((stream.size() % 4U) != 0U) return failure("PM4 stream has a partial dword", 0U);
    const auto total_words = stream.size() / 4U;
    std::vector<RegisterWrite> writes;
    std::size_t next_packet_word = 0U;

    for (const auto& packet : packets) {
        if (packet.header_word_offset != next_packet_word) {
            return failure("PM4 packet list does not cover the stream in order",
                           packet.header_word_offset);
        }
        if (packet.header_word_offset >= total_words) {
            return failure("PM4 packet header is outside the stream", packet.header_word_offset);
        }
        const auto header_offset = packet.header_word_offset * 4U;
        if (read_u32_be(stream, header_offset) != packet.header ||
            packet.payload_word_offset != packet.header_word_offset + 1U) {
            return failure("PM4 packet metadata does not match the supplied stream",
                           packet.header_word_offset);
        }

        const auto header_type = packet.header >> 30U;
        const auto count_field = ((packet.header >> 16U) & 0x3fffU) + 1U;
        const auto expected_type = packet.header == 0U
                                       ? PacketType::empty_nop
                                       : header_type == 0U ? PacketType::type0_registers
                                       : header_type == 1U ? PacketType::type1_registers
                                       : header_type == 2U ? PacketType::type2_nop
                                                           : PacketType::type3_command;
        if (packet.type != expected_type) {
            return failure("PM4 packet type does not match its header", packet.header_word_offset);
        }

        std::uint32_t expected_payload_count = 0U;
        if (expected_type == PacketType::type0_registers ||
            expected_type == PacketType::type3_command) {
            expected_payload_count = count_field;
        } else if (expected_type == PacketType::type1_registers) {
            expected_payload_count = 2U;
        }
        if (packet.payload_word_count != expected_payload_count) {
            return failure("PM4 packet payload count does not match its header",
                           packet.header_word_offset);
        }
        if (packet.payload_word_offset > total_words ||
            packet.payload_word_count > total_words - packet.payload_word_offset) {
            return failure("PM4 register packet payload is outside the stream",
                           packet.header_word_offset);
        }
        next_packet_word = packet.payload_word_offset + packet.payload_word_count;

        if (packet.type == PacketType::type0_registers) {
            if (packet.register_index != (packet.header & 0x7fffU) ||
                packet.write_one_register != ((packet.header & 0x8000U) != 0U)) {
                return failure("PM4 type-0 register metadata does not match its header",
                               packet.header_word_offset);
            }
        } else if (packet.type == PacketType::type1_registers) {
            if (packet.register_index != (packet.header & 0x7ffU) ||
                packet.second_register_index != ((packet.header >> 11U) & 0x7ffU)) {
                return failure("PM4 type-1 register metadata does not match its header",
                               packet.header_word_offset);
            }
        } else if (packet.type == PacketType::type3_command) {
            const auto encoded_opcode = static_cast<std::uint8_t>((packet.header >> 8U) & 0x7fU);
            if (packet.opcode != encoded_opcode || packet.predicated != ((packet.header & 1U) != 0U)) {
                return failure("PM4 type-3 metadata does not match its header",
                               packet.header_word_offset);
            }
        }

        std::size_t write_count = 0U;
        std::size_t payload_values_offset = packet.payload_word_offset;
        std::size_t register_base = 0U;
        bool repeated_target = false;
        if (packet.type == PacketType::type0_registers) {
            write_count = packet.payload_word_count;
            register_base = packet.register_index;
            repeated_target = packet.write_one_register;
        } else if (packet.type == PacketType::type1_registers) {
            write_count = packet.payload_word_count;
        } else if (packet.type == PacketType::type3_command &&
                   (packet.opcode == 0x2dU || packet.opcode == 0x55U ||
                    packet.opcode == 0x56U)) {
            if (packet.predicated) {
                return failure("Predicated PM4 commands require bin-selection state",
                               packet.header_word_offset);
            }
            if (packet.payload_word_count == 0U) {
                return failure("PM4 constant packet is missing its offset dword",
                               packet.header_word_offset);
            }
            const auto offset_type = read_u32_be(stream, packet.payload_word_offset * 4U);
            if (packet.opcode == 0x2dU) {
                const auto index = static_cast<std::size_t>(offset_type & 0x7ffU);
                const auto type = (offset_type >> 16U) & 0xffU;
                switch (type) {
                case 0U: register_base = 0x4000U + index; break; // ALU constants
                case 1U: register_base = 0x4800U + index; break; // Fetch constants
                case 2U: register_base = 0x4900U + index; break; // Boolean constants
                case 3U: register_base = 0x4908U + index; break; // Loop constants
                case 4U: register_base = 0x2000U + index; break; // Direct registers
                default:
                    return failure("PM4_SET_CONSTANT has an unsupported constant type",
                                   packet.header_word_offset);
                }
            } else {
                register_base = static_cast<std::size_t>(offset_type & 0xffffU);
            }
            write_count = packet.payload_word_count - 1U;
            payload_values_offset += 1U;
        } else if (packet.type == PacketType::type3_command && packet.opcode == 0x10U) {
            // PM4_NOP carries optional payload words used as padding or comments.
            continue;
        } else if (packet.type == PacketType::type3_command && packet.opcode == 0x26U) {
            // The current frontend applies packets synchronously and has no GPU queue,
            // so WAIT_FOR_IDLE is already satisfied once prior packets are applied.
            if (packet.predicated || packet.payload_word_count != 1U) {
                return failure("PM4_WAIT_FOR_IDLE requires one unpredicated payload dword",
                               packet.header_word_offset);
            }
            continue;
        } else if (packet.type == PacketType::type3_command) {
            return failure("PM4 type-3 opcode is not implemented by register application",
                           packet.header_word_offset);
        } else {
            continue;
        }

        for (std::size_t i = 0; i < write_count; ++i) {
            std::size_t register_index = register_base;
            if (packet.type == PacketType::type1_registers) {
                register_index = i == 0U ? packet.register_index : packet.second_register_index;
            } else if (!repeated_target) {
                register_index += i;
            }
            if (register_index >= registers.size()) {
                return failure("PM4 register index exceeds the configured Xenos register file",
                               packet.header_word_offset);
            }

            const auto stream_offset = (payload_values_offset + i) * 4U;
            writes.push_back({register_index, read_u32_be(stream, stream_offset)});
        }
    }

    if (next_packet_word != total_words) {
        return failure("PM4 packet list does not cover the complete stream", next_packet_word);
    }

    for (const auto& write : writes) registers[write.index] = write.value;
    return {};
}

} // namespace project360::gpu::xenos
