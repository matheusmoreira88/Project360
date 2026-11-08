#pragma once

#include <array>
#include <cstdint>

namespace project360::gpu::xenos {

// Decoded fields from one three-dword Xenos ALU instruction. Opcode values and
// source encodings stay raw until their operation-specific semantics are
// implemented and validated.
struct ShaderAluInstruction {
    std::uint8_t vector_destination{};
    std::uint8_t scalar_destination{};
    bool vector_destination_relative{};
    bool scalar_destination_relative{};
    bool absolute_constants{};
    bool export_result{};
    std::uint8_t vector_write_mask{};
    std::uint8_t scalar_write_mask{};
    bool vector_clamp{};
    bool scalar_clamp{};
    std::uint8_t scalar_opcode{};

    std::array<std::uint8_t, 3> source_swizzles{};
    std::array<bool, 3> source_negate{};
    bool predicated{};
    bool predicate_condition{};
    bool constant_address_register_relative{};
    bool constant_1_relative{};
    bool constant_0_relative{};

    std::array<std::uint8_t, 3> source_registers{};
    std::uint8_t vector_opcode{};
    std::array<bool, 3> source_is_temporary{};
};

// `words` contains three numeric dwords after the caller has applied the
// guest memory's byte order. This function only decodes fields; it does not
// execute the instruction or imply support for its opcode.
[[nodiscard]] ShaderAluInstruction decode_shader_alu_instruction(
    const std::array<std::uint32_t, 3>& words) noexcept;

} // namespace project360::gpu::xenos
