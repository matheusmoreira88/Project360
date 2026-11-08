#include "gpu/xenos/shader_execution.hpp"

#include "gpu/xenos/vertex_fetch.hpp"
#include "memory/guest_memory.hpp"

#include <array>
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

ShaderExecPairDecodeResult decode_shader_exec_pairs(
    std::span<const std::array<std::uint32_t, 3>> packed_pairs) {
    constexpr std::size_t maximum_control_flow_instructions = 4096U;
    if (packed_pairs.empty()) {
        return {{}, "Xenos shader has no packed control-flow instructions"};
    }
    if (packed_pairs.size() > maximum_control_flow_instructions / 2U) {
        return {{}, "Xenos shader control flow exceeds the 4,096-instruction limit"};
    }

    ShaderExecPairDecodeResult result;
    try {
        result.instructions.reserve(packed_pairs.size() * 2U);
        for (const auto& packed : packed_pairs) {
            const std::array<std::uint32_t, 2> first{packed[0], packed[1] & 0xffffU};
            const std::array<std::uint32_t, 2> second{
                (packed[1] >> 16U) | (packed[2] << 16U), packed[2] >> 16U};
            result.instructions.push_back(decode_shader_exec_instruction(first));
            result.instructions.push_back(decode_shader_exec_instruction(second));
        }
    } catch (const std::bad_alloc&) {
        return {{}, "Not enough host memory to decode Xenos shader control flow"};
    }
    return result;
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

ShaderExecSequenceResult execute_shader_exec_sequence(
    std::span<const ShaderExecInstruction> blocks,
    std::span<const std::array<std::uint32_t, 3>> ucode,
    std::span<const VertexFetchDescriptor> fetch_constants,
    const memory::GuestMemory& memory,
    const ShaderAluTemporaryRegisterBank& initial_registers,
    const std::array<float, 4>& missing_component_values,
    std::span<const std::array<float, 4>> float_constants,
    float initial_previous_scalar,
    std::span<const bool> boolean_constants,
    ShaderVertexOutputs initial_vertex_outputs) {
    constexpr std::size_t maximum_block_count = 4096U;
    if (blocks.empty()) {
        return {{}, 0U, 0U, false, "Xenos shader EXEC sequence is empty"};
    }
    if (blocks.size() > maximum_block_count) {
        return {{}, 0U, 0U, false, "Xenos shader EXEC sequence exceeds the 4,096-block limit"};
    }

    auto registers = initial_registers;
    auto previous_scalar = initial_previous_scalar;
    auto vertex_outputs = initial_vertex_outputs;
    std::uint32_t instructions_executed = 0U;
    std::uint16_t blocks_executed = 0U;
    for (std::size_t block_index = 0U; block_index < blocks.size(); ++block_index) {
        const auto block = execute_shader_exec_block(
            blocks[block_index], ucode, fetch_constants, memory, registers,
            missing_component_values, float_constants, previous_scalar,
            boolean_constants, vertex_outputs);
        if (!block) {
            return {{}, 0U, 0U, false,
                    "Xenos shader EXEC block " + std::to_string(block_index) + ": " + block.error};
        }
        instructions_executed += block.instructions_executed;
        ++blocks_executed;
        registers = block.registers;
        previous_scalar = block.previous_scalar;
        vertex_outputs = block.vertex_outputs;
        if (block.ended_shader) {
            return {registers, instructions_executed, blocks_executed, true, {},
                    previous_scalar, vertex_outputs};
        }
    }
    return {{}, 0U, 0U, false,
            "Xenos shader EXEC sequence ended without EXEC_END or matching COND_EXEC_END"};
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
    return execute_shader_exec_vertex_batch(
        std::span<const ShaderExecInstruction>(&exec, 1U), ucode, fetch_constants,
        memory, draw_vertex_indices, initial_registers, missing_component_values,
        float_constants, boolean_constants);
}

ShaderVertexBatchResult execute_shader_exec_vertex_batch(
    std::span<const ShaderExecInstruction> blocks,
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
            const auto vertex = execute_shader_exec_sequence(
                blocks, ucode, fetch_constants, memory, vertex_registers,
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

ShaderVertexBatchResult execute_pm4_draw_vertex_batch(
    const PreparedDrawCommand& draw, std::span<const ShaderExecInstruction> blocks,
    std::span<const std::array<std::uint32_t, 3>> ucode, const memory::GuestMemory& memory,
    const ShaderAluTemporaryRegisterBank& initial_registers,
    const std::array<float, 4>& missing_component_values,
    std::span<const bool> boolean_constants) {
    constexpr std::size_t maximum_block_count = 4096U;
    if (blocks.empty()) return {{}, "Xenos shader EXEC sequence is empty"};
    if (blocks.size() > maximum_block_count) {
        return {{}, "Xenos shader EXEC sequence exceeds the 4,096-block limit"};
    }
    if (draw.register_snapshot.empty()) {
        return {{}, "Prepared PM4 draw has no retained register snapshot"};
    }

    std::array<bool, 96> required_fetch_slots{};
    std::array<bool, 256> required_alu_constants{};
    bool reached_shader_end = false;
    for (const auto& block : blocks) {
        const bool unconditional = block.opcode == 1U || block.opcode == 2U;
        const bool conditional = block.opcode == 3U || block.opcode == 4U;
        if (!unconditional && !conditional) {
            return {{}, "Unsupported Xenos shader control-flow opcode"};
        }
        if (conditional) {
            if (!block.absolute_addressing) {
                return {{}, "Relative Xenos conditional EXEC addresses are not implemented"};
            }
            if (block.boolean_constant_address >= boolean_constants.size()) {
                return {{}, "Xenos conditional EXEC references a missing bool constant"};
            }
            if (boolean_constants[block.boolean_constant_address] != block.condition) continue;
        }
        if (block.instruction_count > 7U) {
            return {{}, "Xenos EXEC instruction count exceeds its encoded field"};
        }
        const auto start = static_cast<std::size_t>(block.instruction_address);
        const auto count = static_cast<std::size_t>(block.instruction_count);
        if (start > ucode.size() || count > ucode.size() - start) {
            return {{}, "Xenos EXEC instruction range exceeds shader microcode"};
        }

        for (std::size_t slot = 0U; slot < count; ++slot) {
            const auto& words = ucode[start + slot];
            const auto operation_kind = (block.sequence >> (slot * 2U)) & 1U;
            if (operation_kind != 0U) {
                const auto decoded = decode_vertex_fetch_instruction(words);
                if (!decoded) {
                    return {{}, "Xenos EXEC fetch slot " + std::to_string(slot) + ": " +
                                    decoded.error};
                }
                required_fetch_slots[decoded.instruction.fetch_constant_index] = true;
                continue;
            }

            const auto instruction = decode_shader_alu_instruction(words);
            for (std::size_t source = 0U; source < instruction.source_is_temporary.size();
                 ++source) {
                if (!instruction.source_is_temporary[source]) {
                    required_alu_constants[instruction.source_registers[source]] = true;
                }
            }
        }
        if (block.opcode == 2U || block.opcode == 4U) {
            reached_shader_end = true;
            break;
        }
    }
    if (!reached_shader_end) {
        return {{}, "Xenos shader EXEC sequence ended without EXEC_END or matching COND_EXEC_END"};
    }

    std::array<std::uint8_t, 96> fetch_slots{};
    std::size_t fetch_slot_count = 0U;
    for (std::size_t slot = 0U; slot < required_fetch_slots.size(); ++slot) {
        if (required_fetch_slots[slot]) {
            fetch_slots[fetch_slot_count++] = static_cast<std::uint8_t>(slot);
        }
    }
    const auto fetch_bank = decode_vertex_fetch_descriptor_bank(
        draw.register_snapshot,
        std::span<const std::uint8_t>(fetch_slots.data(), fetch_slot_count));
    if (!fetch_bank) return {{}, "PM4 draw VFETCH resources: " + fetch_bank.error};

    std::array<std::uint8_t, 256> alu_constants{};
    std::size_t alu_constant_count = 0U;
    for (std::size_t constant = 0U; constant < required_alu_constants.size(); ++constant) {
        if (required_alu_constants[constant]) {
            alu_constants[alu_constant_count++] = static_cast<std::uint8_t>(constant);
        }
    }
    const auto alu_bank = decode_shader_alu_float_constant_bank(
        draw.register_snapshot,
        std::span<const std::uint8_t>(alu_constants.data(), alu_constant_count));
    if (!alu_bank) return {{}, "PM4 draw ALU resources: " + alu_bank.error};

    return execute_shader_exec_vertex_batch(
        blocks, ucode, fetch_bank.descriptors, memory, draw.geometry.indices,
        initial_registers, missing_component_values, alu_bank.constants, boolean_constants);
}

} // namespace project360::gpu::xenos
