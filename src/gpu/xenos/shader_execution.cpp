#include "gpu/xenos/shader_execution.hpp"

#include "gpu/xenos/vertex_fetch.hpp"
#include "memory/guest_memory.hpp"

#include <cstddef>
#include <new>
#include <string>
#include <utility>

namespace project360::gpu::xenos {

ShaderExecInstruction decode_shader_exec_instruction(
    const std::array<std::uint32_t, 2>& words) noexcept {
    const auto word0 = words[0];
    const auto word1 = words[1];
    return {
        static_cast<std::uint16_t>(word0 & 0x0fffU),
        static_cast<std::uint8_t>((word0 >> 12U) & 0x07U),
        static_cast<std::uint16_t>((word0 >> 16U) & 0x0fffU),
        static_cast<std::uint8_t>((word1 >> 12U) & 0x0fU),
        static_cast<std::uint8_t>(word1 & 0xffU),
        ((word0 >> 15U) & 1U) != 0U,
        ((word1 >> 9U) & 1U) != 0U,
        ((word1 >> 11U) & 1U) != 0U,
        ((word1 >> 8U) & 1U) != 0U,
    };
}

ShaderExecResult execute_shader_exec_block(
    const ShaderExecInstruction& exec,
    std::span<const std::array<std::uint32_t, 3>> ucode,
    std::span<const VertexFetchDescriptor> fetch_constants,
    const memory::GuestMemory& memory,
    const ShaderAluTemporaryRegisterBank& initial_registers,
    const std::array<float, 4>& missing_component_values,
    std::span<const std::array<float, 4>> float_constants,
    float initial_previous_scalar,
    std::span<const bool> boolean_constants,
    ShaderVertexOutputs initial_vertex_outputs) {
    const bool unconditional = exec.opcode == 1U || exec.opcode == 2U;
    const bool conditional = exec.opcode == 3U || exec.opcode == 4U;
    if (!unconditional && !conditional) {
        return {{}, 0U, false, "Unsupported Xenos shader control-flow opcode"};
    }
    if (conditional) {
        if (!exec.absolute_addressing) {
            return {{}, 0U, false, "Relative Xenos conditional EXEC addresses are not implemented"};
        }
        if (exec.boolean_constant_address >= boolean_constants.size()) {
            return {{}, 0U, false, "Xenos conditional EXEC references a missing bool constant"};
        }
        const bool condition_met =
            boolean_constants[exec.boolean_constant_address] == exec.condition;
        if (!condition_met) {
            return {initial_registers, 0U, false, {}, initial_previous_scalar,
                    initial_vertex_outputs};
        }
    }
    if (exec.instruction_count > 7U) {
        return {{}, 0U, false, "Xenos EXEC instruction count exceeds its encoded field"};
    }
    const auto start = static_cast<std::size_t>(exec.instruction_address);
    const auto count = static_cast<std::size_t>(exec.instruction_count);
    if (start > ucode.size() || count > ucode.size() - start) {
        return {{}, 0U, false, "Xenos EXEC instruction range exceeds shader microcode"};
    }

    auto registers = initial_registers;
    auto previous_scalar = initial_previous_scalar;
    auto vertex_outputs = initial_vertex_outputs;
    for (std::size_t slot = 0U; slot < count; ++slot) {
        const auto& words = ucode[start + slot];
        const auto operation_kind = (exec.sequence >> (slot * 2U)) & 1U;
        if (operation_kind == 0U) {
            const auto instruction = decode_shader_alu_instruction(words);
            const auto executed = execute_shader_alu_add_mul_pair(
                instruction, registers, float_constants, previous_scalar, vertex_outputs);
            if (!executed) {
                return {{}, 0U, false,
                        "Xenos EXEC ALU slot " + std::to_string(slot) + ": " + executed.error};
            }
            registers = executed.registers;
            previous_scalar = executed.previous_scalar;
            vertex_outputs = executed.vertex_outputs;
            continue;
        }

        const auto decoded = decode_vertex_fetch_instruction(words);
        if (!decoded) {
            return {{}, 0U, false,
                    "Xenos EXEC fetch slot " + std::to_string(slot) + ": " + decoded.error};
        }
        const std::array<VertexFetchInstruction, 1> instruction{decoded.instruction};
        const auto fetched = execute_vertex_fetch_instructions(
            instruction, fetch_constants, memory, registers, missing_component_values);
        if (!fetched) {
            return {{}, 0U, false,
                    "Xenos EXEC fetch slot " + std::to_string(slot) + ": " + fetched.error};
        }
        registers = fetched.registers;
    }

    return {registers, exec.instruction_count, exec.opcode == 2U || exec.opcode == 4U, {},
            previous_scalar, vertex_outputs};
}

ShaderVertexBatchResult execute_shader_exec_vertex_batch(
    const ShaderExecInstruction& exec,
    std::span<const std::array<std::uint32_t, 3>> ucode,
    std::span<const VertexFetchDescriptor> fetch_constants,
    const memory::GuestMemory& memory,
    std::span<const std::uint32_t> draw_vertex_indices,
    const ShaderAluTemporaryRegisterBank& initial_registers,
    const std::array<float, 4>& missing_component_values,
    std::span<const std::array<float, 4>> float_constants,
    std::span<const bool> boolean_constants) {
    constexpr std::size_t maximum_vertex_count = 65'536U;
    constexpr std::uint32_t maximum_xenos_vertex_index = 0x00ffffffU;
    if (exec.opcode != 2U && exec.opcode != 4U) {
        return {{}, "Xenos vertex batch requires a shader-ending EXEC_END block"};
    }
    if (draw_vertex_indices.empty()) {
        return {{}, "Xenos vertex batch has no draw indexes"};
    }
    if (draw_vertex_indices.size() > maximum_vertex_count) {
        return {{}, "Xenos vertex batch exceeds the 65,536-vertex limit"};
    }

    ShaderVertexBatchResult result;
    try {
        result.vertices.reserve(draw_vertex_indices.size());
        for (std::size_t vertex_number = 0U; vertex_number < draw_vertex_indices.size();
             ++vertex_number) {
            const auto vertex_index = draw_vertex_indices[vertex_number];
            if (vertex_index > maximum_xenos_vertex_index) {
                return {{}, "Xenos vertex batch index exceeds the remapped 24-bit range"};
            }

            auto vertex_registers = initial_registers;
            vertex_registers[0U][0U] = static_cast<float>(vertex_index);
            const auto vertex = execute_shader_exec_block(
                exec, ucode, fetch_constants, memory, vertex_registers,
                missing_component_values, float_constants, 0.0F, boolean_constants);
            if (!vertex) {
                return {{}, "Xenos vertex batch item " + std::to_string(vertex_number) +
                                " (index " + std::to_string(vertex_index) + "): " + vertex.error};
            }
            if (!vertex.ended_shader) {
                return {{}, "Xenos vertex batch item " + std::to_string(vertex_number) +
                                " did not reach shader end"};
            }
            result.vertices.push_back(vertex.vertex_outputs);
        }
    } catch (const std::bad_alloc&) {
        return {{}, "Not enough host memory for Xenos vertex batch output"};
    }
    return result;
}

} // namespace project360::gpu::xenos
