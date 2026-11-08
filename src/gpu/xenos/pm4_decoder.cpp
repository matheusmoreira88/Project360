#include "gpu/xenos/pm4_decoder.hpp"

#include <utility>

namespace project360::gpu::xenos {
namespace {

std::uint32_t read_u32_be(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

DecodeResult failure(std::string message, std::size_t word_offset) {
    return {{}, std::move(message), word_offset};
}

} // namespace

DecodeResult decode_pm4(std::span<const std::uint8_t> bytes) {
    if ((bytes.size() % 4U) != 0U) {
        return failure("PM4 stream length is not a whole number of dwords", bytes.size() / 4U);
    }

    DecodeResult result{};
    const auto total_words = bytes.size() / 4U;
    std::size_t word_offset = 0;
    while (word_offset < total_words) {
        const auto header = read_u32_be(bytes, word_offset * 4U);
        const auto type = header >> 30U;
        Packet packet{};
        packet.header = header;
        packet.header_word_offset = word_offset;
        packet.payload_word_offset = word_offset + 1U;

        if (header == 0U) {
            packet.type = PacketType::empty_nop;
        } else if (type == 0U) {
            packet.type = PacketType::type0_registers;
            packet.payload_word_count = ((header >> 16U) & 0x3fffU) + 1U;
            packet.write_one_register = ((header >> 15U) & 1U) != 0U;
            packet.register_index = static_cast<std::uint16_t>(header & 0x7fffU);
        } else if (type == 1U) {
            packet.type = PacketType::type1_registers;
            packet.payload_word_count = 2U;
            packet.register_index = static_cast<std::uint16_t>(header & 0x7ffU);
            packet.second_register_index = static_cast<std::uint16_t>((header >> 11U) & 0x7ffU);
        } else if (type == 2U) {
            packet.type = PacketType::type2_nop;
        } else {
            packet.type = PacketType::type3_command;
            packet.payload_word_count = ((header >> 16U) & 0x3fffU) + 1U;
            packet.opcode = static_cast<std::uint8_t>((header >> 8U) & 0x7fU);
            packet.predicated = (header & 1U) != 0U;
        }

        const auto words_after_header = total_words - word_offset - 1U;
        if (packet.payload_word_count > words_after_header) {
            return failure("PM4 packet payload is truncated", word_offset);
        }
        word_offset += 1U + packet.payload_word_count;
        result.packets.push_back(packet);
    }
    return result;
}

} // namespace project360::gpu::xenos
