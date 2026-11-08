#include "gpu/xenos/pm4_draw.hpp"

#include "memory/guest_memory.hpp"

#include <new>
#include <utility>

namespace project360::gpu::xenos {
namespace {

constexpr std::uint8_t kDrawIndx = 0x22U;
constexpr std::uint8_t kDrawIndx2 = 0x36U;
constexpr std::size_t kVgtDmaBaseRegister = 0x21faU;
constexpr std::size_t kVgtDmaSizeRegister = 0x21fbU;
constexpr std::size_t kVgtDrawInitiatorRegister = 0x21fcU;
constexpr std::uint32_t kDmaSizeWordMask = 0x00ffffffU;

std::uint32_t swap_index_32(std::uint32_t value, std::uint32_t mode) {
    switch (mode) {
    case 0U: return value;
    case 1U: return ((value & 0x00ff00ffU) << 8U) | ((value & 0xff00ff00U) >> 8U);
    case 2U:
        return ((value & 0x000000ffU) << 24U) | ((value & 0x0000ff00U) << 8U) |
               ((value & 0x00ff0000U) >> 8U) | ((value & 0xff000000U) >> 24U);
    case 3U: return (value << 16U) | (value >> 16U);
    default: return value;
    }
}

std::uint32_t read_u32_be(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

DrawDecodeResult failure(std::string message, std::size_t word_offset) {
    return {{}, std::move(message), word_offset};
}

} // namespace

DrawDecodeResult decode_draw_commands(std::span<const Packet> packets,
                                      std::span<const std::uint8_t> stream) {
    if ((stream.size() % 4U) != 0U) {
        return failure("PM4 stream length is not a whole number of dwords", stream.size() / 4U);
    }

    const auto total_words = stream.size() / 4U;
    std::size_t next_word = 0;
    DrawDecodeResult result{};
    for (const auto& packet : packets) {
        if (packet.header_word_offset != next_word || next_word >= total_words ||
            packet.header != read_u32_be(stream, next_word * 4U)) {
            return failure("PM4 packet metadata does not match the supplied stream", next_word);
        }
        const auto header = packet.header;
        const auto type = header >> 30U;
        const auto expected_count = type == 0U || type == 3U
                                        ? ((header >> 16U) & 0x3fffU) + 1U
                                        : type == 1U ? 2U : 0U;
        if (packet.payload_word_offset != next_word + 1U ||
            packet.payload_word_count != expected_count ||
            packet.payload_word_count > total_words - next_word - 1U) {
            return failure("PM4 packet bounds do not match the supplied stream", next_word);
        }
        const auto expected_type =
            header == 0U ? PacketType::empty_nop
            : type == 0U ? PacketType::type0_registers
            : type == 1U ? PacketType::type1_registers
            : type == 2U ? PacketType::type2_nop
                         : PacketType::type3_command;
        if (packet.type != expected_type ||
            (type == 3U && (packet.opcode != ((header >> 8U) & 0x7fU) ||
                            packet.predicated != ((header & 1U) != 0U)))) {
            return failure("PM4 packet metadata does not match its header", next_word);
        }

        if (packet.type == PacketType::type3_command &&
            (packet.opcode == kDrawIndx || packet.opcode == kDrawIndx2)) {
            if (packet.predicated) {
                return failure("Predicated draw requires visibility-query state", next_word);
            }
            const bool has_query = packet.opcode == kDrawIndx;
            const auto prefix_words = has_query ? 1U : 0U;
            if (packet.payload_word_count < prefix_words + 1U) {
                return failure("Draw packet is missing its initiator", next_word);
            }

            const auto payload = (next_word + 1U) * 4U;
            DrawCommand command{};
            command.header_word_offset = next_word;
            command.opcode = packet.opcode;
            command.has_viz_query_condition = has_query;
            std::size_t cursor = 0;
            if (has_query) {
                command.viz_query_condition = read_u32_be(stream, payload + cursor++ * 4U);
            }
            command.initiator = read_u32_be(stream, payload + cursor++ * 4U);
            command.primitive_type = static_cast<std::uint8_t>(command.initiator & 0x3fU);
            const auto source = static_cast<std::uint8_t>((command.initiator >> 6U) & 0x3U);
            if (source > 2U) {
                return failure("Draw packet uses an invalid source selector", next_word);
            }
            command.source = static_cast<DrawSource>(source);
            command.major_mode = static_cast<std::uint8_t>((command.initiator >> 8U) & 0x3U);
            command.index_32_bit = ((command.initiator >> 11U) & 1U) != 0U;
            command.index_count = static_cast<std::uint16_t>(command.initiator >> 16U);

            const auto remaining = packet.payload_word_count - cursor;
            if (command.source == DrawSource::immediate) {
                return failure("Immediate-index draws are not decoded yet", next_word);
            }
            if (command.source == DrawSource::dma) {
                if (remaining != 2U) {
                    return failure("DMA draw requires base and size dwords", next_word);
                }
                command.dma_base = read_u32_be(stream, payload + cursor++ * 4U);
                command.dma_size = read_u32_be(stream, payload + cursor * 4U);
            } else if (remaining != 0U) {
                return failure("Auto-index draw has unexpected trailing payload", next_word);
            }
            result.commands.push_back(command);
        }
        next_word += 1U + packet.payload_word_count;
    }
    if (next_word != total_words) {
        return failure("PM4 packet list does not cover the complete stream", next_word);
    }
    return result;
}

DrawStateApplyResult apply_draw_command_state(std::span<const DrawCommand> commands,
                                              std::span<std::uint32_t> registers) {
    for (const auto& command : commands) {
        if ((command.opcode != kDrawIndx && command.opcode != kDrawIndx2) ||
            command.has_viz_query_condition != (command.opcode == kDrawIndx)) {
            return {"Draw command metadata is inconsistent", command.header_word_offset};
        }
        const auto source = (command.initiator >> 6U) & 0x3U;
        if (source > static_cast<std::uint32_t>(DrawSource::auto_index) ||
            source != static_cast<std::uint32_t>(command.source) ||
            command.primitive_type != (command.initiator & 0x3fU) ||
            command.major_mode != ((command.initiator >> 8U) & 0x3U) ||
            command.index_32_bit != (((command.initiator >> 11U) & 1U) != 0U) ||
            command.index_count != static_cast<std::uint16_t>(command.initiator >> 16U)) {
            return {"Draw command fields do not match the initiator", command.header_word_offset};
        }
        if (kVgtDrawInitiatorRegister >= registers.size() ||
            (command.source == DrawSource::dma &&
             (kVgtDmaBaseRegister >= registers.size() ||
              kVgtDmaSizeRegister >= registers.size()))) {
            return {"Draw state register exceeds the configured Xenos register span",
                    command.header_word_offset};
        }
    }

    for (const auto& command : commands) {
        registers[kVgtDrawInitiatorRegister] = command.initiator;
        if (command.source == DrawSource::dma) {
            registers[kVgtDmaBaseRegister] = command.dma_base;
            registers[kVgtDmaSizeRegister] = command.dma_size;
        }
    }
    return {};
}

IndexFetchResult fetch_draw_indices(const DrawCommand& command,
                                    const memory::GuestMemory& memory) {
    if (command.source == DrawSource::immediate) {
        return {{}, "Immediate-index draws are not supported"};
    }
    if (command.source != DrawSource::dma && command.source != DrawSource::auto_index) {
        return {{}, "Draw command has an invalid index source"};
    }

    IndexFetchResult result{};
    try {
        result.indices.reserve(command.index_count);
        if (command.source == DrawSource::auto_index) {
            for (std::uint32_t index = 0U; index < command.index_count; ++index) {
                result.indices.push_back(index);
            }
            return result;
        }

        const std::size_t element_size = command.index_32_bit ? 4U : 2U;
        const auto available_elements = command.dma_size & kDmaSizeWordMask;
        if (command.index_count > available_elements) {
            return {{}, "Draw requests more indexes than the DMA size permits"};
        }
        const auto endian_mode = command.dma_size >> 30U;
        if (!command.index_32_bit && endian_mode > 1U) {
            return {{}, "32-bit DMA endian mode for 16-bit indexes is unsupported"};
        }

        const auto aligned_base = static_cast<std::uint64_t>(
            command.dma_base & ~static_cast<std::uint32_t>(element_size - 1U));
        const auto byte_count = static_cast<std::size_t>(command.index_count) * element_size;
        std::vector<std::byte> raw_data(byte_count);
        if (!memory.read_physical(aligned_base, raw_data)) {
            return {{}, "DMA index range exceeds physical guest memory"};
        }
        for (std::uint32_t index = 0U; index < command.index_count; ++index) {
            const auto offset = static_cast<std::size_t>(index) * element_size;
            if (command.index_32_bit) {
                const auto raw_value =
                    std::to_integer<std::uint32_t>(raw_data[offset]) |
                    (std::to_integer<std::uint32_t>(raw_data[offset + 1U]) << 8U) |
                    (std::to_integer<std::uint32_t>(raw_data[offset + 2U]) << 16U) |
                    (std::to_integer<std::uint32_t>(raw_data[offset + 3U]) << 24U);
                result.indices.push_back(swap_index_32(raw_value, endian_mode));
            } else {
                auto guest_value = static_cast<std::uint16_t>(
                    std::to_integer<std::uint16_t>(raw_data[offset]) |
                    (std::to_integer<std::uint16_t>(raw_data[offset + 1U]) << 8U));
                if (endian_mode == 1U) {
                    guest_value = static_cast<std::uint16_t>((guest_value << 8U) |
                                                             (guest_value >> 8U));
                }
                result.indices.push_back(guest_value);
            }
        }
    } catch (const std::bad_alloc&) {
        return {{}, "Not enough host memory for the decoded draw indexes"};
    }
    return result;
}

} // namespace project360::gpu::xenos
