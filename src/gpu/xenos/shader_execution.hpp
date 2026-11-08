#pragma once

#include "gpu/xenos/shader_alu.hpp"
#include "gpu/xenos/vertex_fetch.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace project360::memory {
class GuestMemory;
}

namespace project360::gpu::xenos {

struct ShaderExecInstruction {
    std::uint16_t instruction_address{};
    std::uint8_t instruction_count{};
    std::uint16_t sequence{};
    std::uint8_t opcode{};
    std::uint8_t boolean_constant_address{};
    bool yield{};
    bool predicate_clean{};
    bool absolute_addressing{};
    bool condition{};
};

struct ShaderExecResult {
    ShaderAluTemporaryRegisterBank registers{};
    std::uint8_t instructions_executed{};
    bool ended_shader{};
    std::string error;
    float previous_scalar{};
    ShaderVertexOutputs vertex_outputs{};

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct ShaderVertexBatchResult {
    std::vector<ShaderVertexOutputs> vertices;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Decodes the first two dwords of a Xenos control-flow instruction. Opcode
// values remain raw so unsupported control flow can be rejected by execution.
[[nodiscard]] ShaderExecInstruction decode_shader_exec_instruction(
    const std::array<std::uint32_t, 2>& words) noexcept;

// Executes one EXEC/EXEC_END or bool-constant COND_EXEC/COND_EXEC_END block in
// declared sequence order. Conditional blocks require absolute addressing and
// a supplied bool-constant bank. Only ALU pairs and full VFETCH instructions
// are supported. The block commits atomically: errors expose no partially
// updated register bank.
[[nodiscard]] ShaderExecResult execute_shader_exec_block(
    const ShaderExecInstruction& exec,
    std::span<const std::array<std::uint32_t, 3>> ucode,
    std::span<const VertexFetchDescriptor> fetch_constants,
    const memory::GuestMemory& memory,
    const ShaderAluTemporaryRegisterBank& initial_registers,
    const std::array<float, 4>& missing_component_values,
    std::span<const std::array<float, 4>> float_constants = {},
    float initial_previous_scalar = 0.0F,
    std::span<const bool> boolean_constants = {},
    ShaderVertexOutputs initial_vertex_outputs = {});

// Executes one shader-ending EXEC_END / matching COND_EXEC_END block for each
// draw-remapped vertex index. Xenos vertex startup seeds r0.x with that index;
// every vertex receives independent register, scalar and output state. The
// batch is bounded and all-or-error so partial vertex output cannot be drawn.
[[nodiscard]] ShaderVertexBatchResult execute_shader_exec_vertex_batch(
    const ShaderExecInstruction& exec,
    std::span<const std::array<std::uint32_t, 3>> ucode,
    std::span<const VertexFetchDescriptor> fetch_constants,
    const memory::GuestMemory& memory,
    std::span<const std::uint32_t> draw_vertex_indices,
    const ShaderAluTemporaryRegisterBank& initial_registers,
    const std::array<float, 4>& missing_component_values,
    std::span<const std::array<float, 4>> float_constants = {},
    std::span<const bool> boolean_constants = {});

} // namespace project360::gpu::xenos
