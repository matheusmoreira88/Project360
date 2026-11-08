#include "gpu/xenos/shader_execution.hpp"

#include "gpu/xenos/vertex_fetch.hpp"
#include "memory/guest_memory.hpp"

#include <cstddef>
#include <string>

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
        ((word0 >> 15U) & 1U) != 0U,
        ((word1 >> 9U) & 1U) != 0U,
        ((word1 >> 11U) & 1U) != 0U,
    };
}

ShaderExecResult execute_shader_exec_block(
    const ShaderExecInstruction& exec,
    std::span<const std::array<std::uint32_t, 3>> ucode,
    std::span<const VertexFetchDescriptor> fetch_constants,
    const memory::GuestMemory& memory,
    const ShaderAluTemporaryRegisterBank& initial_registers,
    const std::array<float, 4>& missing_component_values,
    std::span<const std::array<float, 4>> float_constants) {
    if (exec.opcode != 1U && exec.opcode != 2U) {
        return {{}, 0U, false, "Only unconditional Xenos EXEC/EXEC_END blocks are supported"};
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
    for (std::size_t slot = 0U; slot < count; ++slot) {
        const auto& words = ucode[start + slot];
        const auto operation_kind = (exec.sequence >> (slot * 2U)) & 1U;
        if (operation_kind == 0U) {
            const auto instruction = decode_shader_alu_instruction(words);
            const auto executed =
                execute_shader_alu_add_mul_pair(instruction, registers, float_constants);
            if (!executed) {
                return {{}, 0U, false,
                        "Xenos EXEC ALU slot " + std::to_string(slot) + ": " + executed.error};
            }
            registers = executed.registers;
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

    return {registers, exec.instruction_count, exec.opcode == 2U, {}};
}

} // namespace project360::gpu::xenos
