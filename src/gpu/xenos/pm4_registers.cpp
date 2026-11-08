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

    for (const auto& packet : packets) {
        if (packet.header_word_offset >= total_words) {
            return failure("PM4 packet header is outside the stream", packet.header_word_offset);
        }
        const auto header_offset = packet.header_word_offset * 4U;
        if (read_u32_be(stream, header_offset) != packet.header ||
            packet.payload_word_offset != packet.header_word_offset + 1U) {
            return failure("PM4 packet metadata does not match the supplied stream",
                           packet.header_word_offset);
        }

        std::size_t write_count = 0;
        if (packet.type == PacketType::type0_registers ||
            packet.type == PacketType::type1_registers) {
            write_count = packet.payload_word_count;
        } else {
            continue;
        }

        const auto header_type = packet.header >> 30U;
        if ((packet.type == PacketType::type0_registers &&
             (packet.header == 0U || header_type != 0U)) ||
            (packet.type == PacketType::type1_registers && header_type != 1U)) {
            return failure("PM4 register packet type does not match its header",
                           packet.header_word_offset);
        }

        if (packet.payload_word_offset > total_words ||
            write_count > total_words - packet.payload_word_offset) {
            return failure("PM4 register packet payload is outside the stream",
                           packet.header_word_offset);
        }
        if (packet.type == PacketType::type1_registers && write_count != 2U) {
            return failure("PM4 type-1 packet must contain exactly two register values",
                           packet.header_word_offset);
        }
        if (packet.type == PacketType::type0_registers && write_count == 0U) {
            return failure("PM4 type-0 packet has an empty register write", packet.header_word_offset);
        }

        for (std::size_t i = 0; i < write_count; ++i) {
            std::size_t register_index = packet.register_index;
            if (packet.type == PacketType::type1_registers) {
                register_index = i == 0U ? packet.register_index : packet.second_register_index;
            } else if (!packet.write_one_register) {
                register_index += i;
            }
            if (register_index >= registers.size()) {
                return failure("PM4 register index exceeds the configured Xenos register file",
                               packet.header_word_offset);
            }

            const auto stream_offset = (packet.payload_word_offset + i) * 4U;
            writes.push_back({register_index, read_u32_be(stream, stream_offset)});
        }
    }

    for (const auto& write : writes) registers[write.index] = write.value;
    return {};
}

} // namespace project360::gpu::xenos
