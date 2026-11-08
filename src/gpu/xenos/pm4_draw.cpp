#include "gpu/xenos/pm4_draw.hpp"

#include "gpu/xenos/registers.hpp"
#include "gpu/xenos/pm4_registers.hpp"
#include "memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <utility>

namespace project360::gpu::xenos {
namespace {

constexpr std::uint8_t kDrawIndx = 0x22U;
constexpr std::uint8_t kDrawIndx2 = 0x36U;
constexpr std::uint8_t kImLoad = 0x27U;
constexpr std::uint8_t kImLoadImmediate = 0x2bU;
constexpr std::uint32_t kDmaSizeWordMask = 0x00ffffffU;
constexpr std::size_t kMaximumDrawSnapshotBytes = 32U * 1024U * 1024U;

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

std::uint32_t read_u32_be(std::span<const std::byte> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U]))
            << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U]))
            << 8U) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U]));
}

struct ShaderMicrocodeLoadResult {
    std::shared_ptr<const ShaderMicrocode> shader;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return shader != nullptr; }
};

ShaderMicrocodeLoadResult decode_shader_microcode(
    std::uint32_t shader_type, std::uint32_t source_address, std::uint32_t start_dword,
    std::uint32_t size_dwords, std::span<const std::byte> source_bytes) {
    constexpr std::uint32_t maximum_size_dwords = 65'535U;
    if (shader_type > static_cast<std::uint32_t>(ShaderStage::pixel)) {
        return {nullptr, "PM4 shader load selects an unsupported shader stage"};
    }
    if (start_dword != 0U) {
        return {nullptr, "PM4 shader load with a nonzero instruction-memory offset is unsupported"};
    }
    if (size_dwords == 0U || size_dwords > maximum_size_dwords || size_dwords % 3U != 0U) {
        return {nullptr, "PM4 shader load size must contain complete three-dword records"};
    }
    const auto expected_bytes = static_cast<std::size_t>(size_dwords) * sizeof(std::uint32_t);
    if (source_bytes.size() != expected_bytes) {
        return {nullptr, "PM4 shader load data size does not match its declared dword count"};
    }

    auto shader = std::make_shared<ShaderMicrocode>();
    shader->stage = static_cast<ShaderStage>(shader_type);
    shader->source_address = source_address;
    shader->records.resize(size_dwords / 3U);
    for (std::size_t dword = 0U; dword < size_dwords; ++dword) {
        shader->records[dword / 3U][dword % 3U] = read_u32_be(source_bytes, dword * 4U);
    }
    return {std::move(shader), {}};
}

DrawDecodeResult failure(std::string message, std::size_t word_offset) {
    return {{}, std::move(message), word_offset};
}

PrimitiveAssemblyResult primitive_failure(std::string message) {
    return {{}, {}, {}, std::move(message)};
}

bool supports_restart(std::uint8_t primitive_type) {
    return primitive_type == 3U || primitive_type == 5U || primitive_type == 6U ||
           primitive_type == 0x0cU;
}

bool is_restart_index(const DrawCommand& command, std::uint32_t index,
                      std::uint32_t reset_index) {
    if (command.index_32_bit) {
        return (index & 0x00ffffffU) == (reset_index & 0x00ffffffU);
    }
    return index == reset_index;
}

void append_range(PrimitiveAssemblyResult& result, std::span<const std::uint32_t> source,
                  std::uint8_t primitive_type) {
    const auto first_index = result.indices.size();
    switch (primitive_type) {
    case 1U: // Point list.
    case 2U: // Line list.
    case 3U: // Line strip.
    case 4U: // Triangle list.
    case 6U: // Triangle strip.
        result.indices.insert(result.indices.end(), source.begin(), source.end());
        break;
    case 5U: // Triangle fan, converted to independent triangles.
        for (std::size_t index = 1U; index + 1U < source.size(); ++index) {
            result.indices.push_back(source[index]);
            result.indices.push_back(source[index + 1U]);
            result.indices.push_back(source[0U]);
        }
        break;
    case 0x0cU: // Line loop, converted to independent line segments.
        if (source.size() >= 2U) {
            for (std::size_t index = 0U; index + 1U < source.size(); ++index) {
                result.indices.push_back(source[index]);
                result.indices.push_back(source[index + 1U]);
            }
            result.indices.push_back(source.back());
            result.indices.push_back(source.front());
        }
        break;
    case 0x0dU: // Quad list, split along the first-to-third diagonal.
        for (std::size_t index = 0U; index + 3U < source.size(); index += 4U) {
            result.indices.push_back(source[index]);
            result.indices.push_back(source[index + 1U]);
            result.indices.push_back(source[index + 2U]);
            result.indices.push_back(source[index]);
            result.indices.push_back(source[index + 2U]);
            result.indices.push_back(source[index + 3U]);
        }
        break;
    default:
        break;
    }
    const auto count = result.indices.size() - first_index;
    if (count != 0U) {
        result.ranges.push_back({first_index, count});
    }
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
        if (register_index::vgt_draw_initiator >= registers.size() ||
            (command.source == DrawSource::dma &&
             (register_index::vgt_dma_base >= registers.size() ||
              register_index::vgt_dma_size >= registers.size()))) {
            return {"Draw state register exceeds the configured Xenos register span",
                    command.header_word_offset};
        }
    }

    for (const auto& command : commands) {
        registers[register_index::vgt_draw_initiator] = normalize_xenos_register_write(
            register_index::vgt_draw_initiator, command.initiator);
        if (command.source == DrawSource::dma) {
            registers[register_index::vgt_dma_base] = normalize_xenos_register_write(
                register_index::vgt_dma_base, command.dma_base);
            registers[register_index::vgt_dma_size] = normalize_xenos_register_write(
                register_index::vgt_dma_size, command.dma_size);
        }
    }
    return {};
}

Pm4DrawStreamResult process_pm4_draw_stream(
    std::span<const std::uint8_t> stream,
    std::span<const std::uint32_t> initial_registers,
    const memory::GuestMemory& memory,
    PrimitiveAssemblyOptions options) {
    constexpr std::uint8_t draw_indx_opcode = 0x22U;
    constexpr std::uint8_t draw_indx2_opcode = 0x36U;
    constexpr std::size_t maximum_prepared_draws = 256U;
    constexpr std::size_t maximum_prepared_indices = 4'194'304U;
    constexpr std::size_t maximum_register_snapshot_bytes = kMaximumDrawSnapshotBytes;

    try {
        const auto decoded = decode_pm4(stream);
        if (!decoded) return {{}, {}, decoded.error, decoded.error_word_offset};

        auto registers = std::vector<std::uint32_t>(initial_registers.begin(),
                                                    initial_registers.end());
        Pm4DrawStreamResult result{};
        std::size_t prepared_index_count = 0U;
        std::size_t prepared_register_snapshot_bytes = 0U;
        std::shared_ptr<const ShaderMicrocode> active_vertex_shader;
        std::shared_ptr<const ShaderMicrocode> active_pixel_shader;
        for (const auto& packet : decoded.packets) {
            const auto byte_offset = packet.header_word_offset * 4U;
            const auto packet_byte_size =
                (static_cast<std::size_t>(packet.payload_word_count) + 1U) * 4U;
            auto packet_stream = stream.subspan(byte_offset, packet_byte_size);
            auto local_packet = packet;
            local_packet.header_word_offset = 0U;
            local_packet.payload_word_offset = 1U;
            const std::array<Packet, 1> packet_list{local_packet};

            if (packet.type == PacketType::type3_command &&
                (packet.opcode == kImLoad || packet.opcode == kImLoadImmediate)) {
                if (packet.predicated) {
                    return {{}, {}, "Predicated PM4 shader loads require bin-selection state",
                            packet.header_word_offset};
                }

                std::uint32_t shader_type = 0U;
                std::uint32_t source_address = 0U;
                std::uint32_t start_dword = 0U;
                std::uint32_t size_dwords = 0U;
                std::span<const std::byte> inline_bytes;
                std::vector<std::byte> physical_bytes;

                if (packet.opcode == kImLoad) {
                    if (packet.payload_word_count != 2U) {
                        return {{}, {}, "PM4_IM_LOAD requires an address and start/size dword",
                                packet.header_word_offset};
                    }
                    const auto address_type = read_u32_be(packet_stream, 4U);
                    shader_type = address_type & 0x3U;
                    source_address = (address_type & ~0x3U) & 0x1fffffffU;
                    const auto start_size = read_u32_be(packet_stream, 8U);
                    start_dword = start_size >> 16U;
                    size_dwords = start_size & 0xffffU;
                    if (size_dwords == 0U || size_dwords % 3U != 0U) {
                        return {{}, {}, "PM4_IM_LOAD size must contain complete three-dword records",
                                packet.header_word_offset};
                    }
                    physical_bytes.resize(static_cast<std::size_t>(size_dwords) * 4U);
                    if (!memory.read_physical(source_address, physical_bytes)) {
                        return {{}, {}, "PM4_IM_LOAD shader bytes exceed guest physical memory",
                                packet.header_word_offset};
                    }
                    inline_bytes = physical_bytes;
                } else {
                    if (packet.payload_word_count < 2U) {
                        return {{}, {}, "PM4_IM_LOAD_IMMEDIATE requires stage and start/size dwords",
                                packet.header_word_offset};
                    }
                    shader_type = read_u32_be(packet_stream, 4U);
                    const auto start_size = read_u32_be(packet_stream, 8U);
                    start_dword = start_size >> 16U;
                    size_dwords = start_size & 0xffffU;
                    if (size_dwords == 0U || size_dwords % 3U != 0U ||
                        static_cast<std::size_t>(size_dwords) + 2U !=
                            packet.payload_word_count) {
                        return {{}, {},
                                "PM4_IM_LOAD_IMMEDIATE payload must match complete three-dword records",
                                packet.header_word_offset};
                    }
                    inline_bytes = std::as_bytes(
                        packet_stream.subspan(12U, static_cast<std::size_t>(size_dwords) * 4U));
                }

                const auto loaded = decode_shader_microcode(shader_type, source_address,
                                                            start_dword, size_dwords,
                                                            inline_bytes);
                if (!loaded) {
                    return {{}, {}, loaded.error, packet.header_word_offset};
                }
                if (loaded.shader->stage == ShaderStage::vertex) {
                    active_vertex_shader = loaded.shader;
                } else {
                    active_pixel_shader = loaded.shader;
                }
                continue;
            }

            if (packet.type != PacketType::type3_command ||
                (packet.opcode != draw_indx_opcode && packet.opcode != draw_indx2_opcode)) {
                const auto applied = apply_register_packets(packet_list, packet_stream, registers);
                if (!applied) {
                    return {{}, {}, applied.error, packet.header_word_offset};
                }
                continue;
            }

            const auto draw_decode = decode_draw_commands(packet_list, packet_stream);
            if (!draw_decode || draw_decode.commands.size() != 1U) {
                return {{}, {}, draw_decode.error.empty()
                                     ? "PM4 draw packet did not produce exactly one draw"
                                     : draw_decode.error,
                        packet.header_word_offset};
            }
            auto command = draw_decode.commands.front();
            command.header_word_offset = packet.header_word_offset;
            const std::array<DrawCommand, 1> command_list{command};
            const auto state_applied = apply_draw_command_state(command_list, registers);
            if (!state_applied) {
                return {{}, {}, state_applied.error, packet.header_word_offset};
            }
            auto geometry = prepare_draw_geometry(command, memory, registers, options);
            if (!geometry) {
                return {{}, {}, "PM4 draw geometry preparation failed: " + geometry.error,
                        packet.header_word_offset};
            }
            if (result.draws.size() >= maximum_prepared_draws) {
                return {{}, {}, "PM4 draw stream exceeds the 256-draw limit",
                        packet.header_word_offset};
            }
            if (geometry.indices.size() > maximum_prepared_indices - prepared_index_count) {
                return {{}, {}, "PM4 draw stream exceeds the 4,194,304-index limit",
                        packet.header_word_offset};
            }
            if (registers.size() > maximum_register_snapshot_bytes / sizeof(std::uint32_t)) {
                return {{}, {}, "PM4 draw register snapshot exceeds the 32 MiB limit",
                        packet.header_word_offset};
            }
            const auto register_snapshot_bytes = registers.size() * sizeof(std::uint32_t);
            const auto vertex_shader_bytes = active_vertex_shader
                                                 ? active_vertex_shader->records.size() *
                                                       sizeof(active_vertex_shader->records[0])
                                                 : 0U;
            const auto pixel_shader_bytes = active_pixel_shader
                                                ? active_pixel_shader->records.size() *
                                                      sizeof(active_pixel_shader->records[0])
                                                : 0U;
            const auto shader_snapshot_bytes = vertex_shader_bytes + pixel_shader_bytes;
            if (register_snapshot_bytes > maximum_register_snapshot_bytes -
                                              prepared_register_snapshot_bytes ||
                shader_snapshot_bytes > maximum_register_snapshot_bytes -
                                            prepared_register_snapshot_bytes -
                                            register_snapshot_bytes) {
                return {{}, {}, "PM4 draw stream exceeds the 32 MiB state/shader snapshot limit",
                        packet.header_word_offset};
            }
            prepared_index_count += geometry.indices.size();
            prepared_register_snapshot_bytes += register_snapshot_bytes + shader_snapshot_bytes;
            result.draws.push_back(
                {std::move(command), std::move(geometry), registers, active_vertex_shader,
                 active_pixel_shader});
        }
        result.final_registers = std::move(registers);
        return result;
    } catch (const std::bad_alloc&) {
        return {{}, {}, "Not enough host memory to process the PM4 draw stream", 0U};
    }
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

PrimitiveAssemblyResult assemble_draw_primitives(
    const DrawCommand& command, std::span<const std::uint32_t> indices,
    PrimitiveAssemblyOptions options) {
    if (indices.size() != command.index_count) {
        return primitive_failure("Fetched index count does not match the draw command");
    }

    PrimitiveAssemblyResult result{};
    switch (command.primitive_type) {
    case 1U: result.topology = PrimitiveTopology::points; break;
    case 2U:
    case 0x0cU: result.topology = PrimitiveTopology::lines; break;
    case 3U: result.topology = PrimitiveTopology::line_strip; break;
    case 4U:
    case 5U:
    case 0x0dU: result.topology = PrimitiveTopology::triangles; break;
    case 6U: result.topology = PrimitiveTopology::triangle_strip; break;
    default: return primitive_failure("Xenos primitive type is not supported yet");
    }
    if (options.restart_enabled && !supports_restart(command.primitive_type)) {
        return primitive_failure("Primitive restart is not defined for this supported topology");
    }

    try {
        if (!options.restart_enabled) {
            append_range(result, indices, command.primitive_type);
            return result;
        }

        std::size_t segment_begin = 0U;
        for (std::size_t index = 0U; index <= indices.size(); ++index) {
            const bool at_end = index == indices.size();
            if (!at_end && !is_restart_index(command, indices[index], options.restart_index)) {
                continue;
            }
            append_range(result, indices.subspan(segment_begin, index - segment_begin),
                         command.primitive_type);
            segment_begin = index + 1U;
        }
    } catch (const std::bad_alloc&) {
        return primitive_failure("Not enough host memory to assemble draw primitives");
    }
    return result;
}

PrimitiveAssemblyResult prepare_draw_geometry(
    const DrawCommand& command, const memory::GuestMemory& memory,
    std::span<const std::uint32_t> registers, PrimitiveAssemblyOptions options) {
    const auto fetched = fetch_draw_indices(command, memory);
    if (!fetched) return primitive_failure(fetched.error);

    auto assembled = assemble_draw_primitives(command, fetched.indices, options);
    if (!assembled) return assembled;

    const auto transform = read_vertex_index_transform(registers);
    if (!transform) return primitive_failure(transform.error);
    for (auto& index : assembled.indices) {
        index = transform_vertex_shader_index(index, transform.transform);
    }
    return assembled;
}

std::uint32_t transform_vertex_shader_index(std::uint32_t vertex_index,
                                            VertexIndexTransform transform) noexcept {
    constexpr std::uint32_t kVertexIndexMask = 0x00ffffffU;
    auto remapped = (vertex_index + transform.offset) & kVertexIndexMask;
    remapped = std::max(remapped, transform.minimum);
    return std::min(remapped, transform.maximum);
}

VertexIndexTransformReadResult read_vertex_index_transform(
    std::span<const std::uint32_t> registers) {
    constexpr auto required_size = register_index::vgt_indx_offset + 1U;
    if (registers.size() < required_size) {
        return {{}, "Register file does not include the Xenos vertex-index state"};
    }
    return {{registers[register_index::vgt_indx_offset] & 0x00ffffffU,
             registers[register_index::vgt_min_vtx_indx] & 0x00ffffffU,
             registers[register_index::vgt_max_vtx_indx] & 0x00ffffffU},
            {}};
}

} // namespace project360::gpu::xenos
