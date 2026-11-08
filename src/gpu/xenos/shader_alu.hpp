#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace project360::gpu::xenos {

// Decoded fields from one three-dword Xenos ALU instruction. Opcode values and
// source encodings stay raw until their operation-specific semantics are
// implemented and validated.
struct ShaderAluInstruction {
    std::uint8_t vector_destination{};
    std::uint8_t scalar_destination{};
    bool vector_destination_relative{};
    bool scalar_destination_relative{};
    bool constant_absolute_value{};
    bool export_result{};
    std::uint8_t vector_write_mask{};
    std::uint8_t scalar_write_mask{};
    bool vector_clamp{};
    bool scalar_clamp{};
    std::uint8_t scalar_opcode{};

    // Encoded field order is src3, src2, src1; the resolver's enum uses the
    // assembly operand names and maps them to these slots.
    std::array<std::uint8_t, 3> source_swizzles{};
    std::array<bool, 3> source_negate{};
    bool predicated{};
    bool predicate_condition{};
    bool constant_address_register_relative{};
    bool constant_1_relative{};
    bool constant_0_relative{};

    // Raw register bytes in src3, src2, src1 order (including relative/abs bits).
    std::array<std::uint8_t, 3> source_registers{};
    std::uint8_t vector_opcode{};
    std::array<bool, 3> source_is_temporary{};
};

struct ShaderVectorAluResult {
    std::array<float, 4> destination{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

enum class ShaderAluSource : std::uint8_t { source_1, source_2, source_3 };

using ShaderAluTemporaryRegisterBank = std::array<std::array<float, 4>, 64>;
using ShaderAluFloatConstantBank = std::array<std::array<float, 4>, 256>;

struct ShaderAluSourceResult {
    std::array<float, 4> value{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct ShaderAluExecutionResult {
    ShaderAluTemporaryRegisterBank registers{};
    float previous_scalar{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// `words` contains three numeric dwords after the caller has applied the
// guest memory's byte order. This function only decodes fields; it does not
// execute the instruction or imply support for its opcode.
[[nodiscard]] ShaderAluInstruction decode_shader_alu_instruction(
    const std::array<std::uint32_t, 3>& words) noexcept;

// Resolves one temporary-register source, including its relative component
// swizzle and abs/negate modifiers. Constant sources are rejected here; use
// resolve_shader_alu_source when float constants are available.
[[nodiscard]] ShaderAluSourceResult resolve_shader_alu_temporary_source(
    const ShaderAluInstruction& instruction, ShaderAluSource source,
    const ShaderAluTemporaryRegisterBank& temporary_registers);

// Resolves an ALU operand from temporary registers or absolute float constants.
// a0/aL-relative constants and loop-relative temporaries fail explicitly.
[[nodiscard]] ShaderAluSourceResult resolve_shader_alu_source(
    const ShaderAluInstruction& instruction, ShaderAluSource source,
    const ShaderAluTemporaryRegisterBank& temporary_registers,
    std::span<const std::array<float, 4>> float_constants);

// Evaluates only the vector half of a decoded instruction. Source vectors must
// already have their encoded source selection, swizzle and negate semantics
// applied. The scalar half is deliberately not evaluated here. Unsupported
// vector control modes fail explicitly instead of being approximated.
[[nodiscard]] ShaderVectorAluResult evaluate_shader_vector_alu(
    const ShaderAluInstruction& instruction, const std::array<float, 4>& source_1,
    const std::array<float, 4>& source_2,
    const std::array<float, 4>& previous_destination);

// Resolves src1/src2 from the temporary bank and evaluates the vector half.
// The caller remains responsible for scalar pairing and destination storage.
[[nodiscard]] ShaderVectorAluResult evaluate_shader_vector_alu_from_temporaries(
    const ShaderAluInstruction& instruction,
    const ShaderAluTemporaryRegisterBank& temporary_registers,
    const std::array<float, 4>& previous_destination,
    std::span<const std::array<float, 4>> float_constants = {});

// Executes the vector ADD/MUL and scalar ADDS/MULS pair for one instruction.
// Both results read the original register bank; writes are committed together.
// Unsupported addressing, exports, clamp and operation pairs fail atomically.
[[nodiscard]] ShaderAluExecutionResult execute_shader_alu_add_mul_pair(
    const ShaderAluInstruction& instruction,
    const ShaderAluTemporaryRegisterBank& initial_registers,
    std::span<const std::array<float, 4>> float_constants = {});

} // namespace project360::gpu::xenos
