#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace project360::gpu::xenos {

enum class PacketType : std::uint8_t {
    empty_nop,
    type0_registers,
    type1_registers,
    type2_nop,
    type3_command
};

struct Packet {
    PacketType type{};
    std::uint32_t header{};
    std::size_t header_word_offset{};
    std::size_t payload_word_offset{};
    std::uint32_t payload_word_count{};
    std::uint16_t register_index{};
    std::uint16_t second_register_index{};
    std::uint8_t opcode{};
    bool write_one_register{};
    bool predicated{};
};

struct DecodeResult {
    std::vector<Packet> packets;
    std::string error;
    std::size_t error_word_offset{};

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Decodes a big-endian PM4 command stream into packet boundaries and header
// metadata. It does not apply register writes or execute type-3 commands.
[[nodiscard]] DecodeResult decode_pm4(std::span<const std::uint8_t> bytes);

} // namespace project360::gpu::xenos
