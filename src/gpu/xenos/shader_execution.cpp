#include "gpu/xenos/shader_execution.hpp"

#include "gpu/xenos/texture_sampling.hpp"
#include "gpu/xenos/vertex_fetch.hpp"
#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <new>
#include <string>
#include <utility>

namespace project360::gpu::xenos {
namespace {

bool resolve_texture_filter(std::uint8_t instruction_filter,
                            std::uint8_t descriptor_filter,
                            TextureSampleFilter& filter) {
    const auto resolved = instruction_filter == 3U ? descriptor_filter : instruction_filter;
    if (resolved == 0U) {
        filter = TextureSampleFilter::point;
        return true;
    }
    if (resolved == 1U) {
        filter = TextureSampleFilter::linear;
        return true;
    }
    return false;
}

bool execute_texture_fetch_instruction(
    const TextureFetchInstruction& instruction,
    std::span<const TextureFetchConstant> texture_constants,
    const memory::GuestMemory& memory, ShaderAluTemporaryRegisterBank& registers,
    std::string& error) {
    if (instruction.opcode != 1U) {
        error = "Unsupported Xenos texture-fetch opcode " +
                std::to_string(instruction.opcode);
        return false;
    }
    if (instruction.source_register_relative || instruction.destination_register_relative) {
        error = "Relative Xenos texture-fetch registers are not implemented";
        return false;
    }
    if (instruction.source_register >= registers.size() ||
        instruction.destination_register >= registers.size()) {
        error = "Xenos texture-fetch register index exceeds the temporary bank";
        return false;
    }
    if (instruction.texture_slot >= texture_constants.size()) {
        error = "Xenos texture fetch references a missing texture constant";
        return false;
    }
    if (instruction.predicated) {
        error = "Predicated Xenos texture fetch requires predicate state";
        return false;
    }
    if (instruction.unnormalized_coordinates || instruction.dimension != 1U) {
        error = "Only normalized 2D Xenos texture coordinates are supported";
        return false;
    }
    if (instruction.use_register_lod || instruction.register_gradients ||
        instruction.offset_x_half_texels != 0 || instruction.offset_y_half_texels != 0 ||
        instruction.offset_z_half_texels != 0 || instruction.sample_location) {
        error = "Xenos texture LOD, gradients, offsets or sample-location mode are unsupported";
        return false;
    }

    const auto& fetch = texture_constants[instruction.texture_slot];
    if (fetch.dimension != TextureDataDimension::two_d_or_stacked || fetch.stacked) {
        error = "Xenos tfetch requires a non-stacked 2D texture descriptor";
        return false;
    }
    if (fetch.mip_min_level != 0U || fetch.mip_max_level != 0U) {
        error = "Xenos tfetch currently supports base-level-only texture descriptors";
        return false;
    }
    TextureSampleFilter magnification_filter{};
    TextureSampleFilter minification_filter{};
    if (!resolve_texture_filter(instruction.magnification_filter,
                                fetch.magnification_filter, magnification_filter) ||
        !resolve_texture_filter(instruction.minification_filter,
                                fetch.minification_filter, minification_filter)) {
        error = "Xenos tfetch filter mode is unsupported";
        return false;
    }
    if (magnification_filter != minification_filter) {
        error = "Different Xenos minification and magnification filters require LOD selection";
        return false;
    }
    const auto anisotropic_filter =
        instruction.anisotropic_filter == 7U ? fetch.anisotropic_filter
                                             : instruction.anisotropic_filter;
    if (anisotropic_filter != 0U || instruction.arbitrary_filter != 0U) {
        error = "Anisotropic or arbitrary Xenos texture filtering is unsupported";
        return false;
    }
    if (instruction.volume_magnification_filter != 0U &&
        instruction.volume_magnification_filter != 3U) {
        error = "Volume magnification filtering is invalid for this 2D tfetch";
        return false;
    }
    if (instruction.volume_minification_filter != 0U &&
        instruction.volume_minification_filter != 3U) {
        error = "Volume minification filtering is invalid for this 2D tfetch";
        return false;
    }

    const auto& coordinate_register = registers[instruction.source_register];
    const auto u_component = static_cast<std::size_t>(instruction.source_swizzle & 0x3U);
    const auto v_component = static_cast<std::size_t>((instruction.source_swizzle >> 2U) & 0x3U);
    const auto sample = sample_xenos_tiled_2d_texture_normalized(
        fetch, coordinate_register[u_component], coordinate_register[v_component],
        magnification_filter, memory);
    if (!sample) {
        error = sample.error;
        return false;
    }

    const std::array<float, 4> sampled_components{
        sample.value.r, sample.value.g, sample.value.b, sample.value.a};
    auto destination = registers[instruction.destination_register];
    for (std::size_t lane = 0U; lane < instruction.destination_swizzle.size(); ++lane) {
        const auto selector = instruction.destination_swizzle[lane];
        if (selector <= 3U) {
            destination[lane] = sampled_components[selector];
        } else if (selector == 4U) {
            destination[lane] = 0.0F;
        } else if (selector == 5U) {
            destination[lane] = 1.0F;
        } else if (selector != 7U) {
            error = "Xenos tfetch destination swizzle uses a reserved selector";
            return false;
        }
    }
    registers[instruction.destination_register] = destination;
    return true;
}

} // namespace

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

ShaderExecProgramDecodeResult decode_shader_exec_program(
    std::span<const std::array<std::uint32_t, 3>> complete_ucode,
    std::uint32_t control_flow_record_count) {
    constexpr std::uint32_t maximum_control_flow_record_count = 2048U;
    if (complete_ucode.empty() || control_flow_record_count == 0U) {
        return {{}, 0U, "Xenos shader microcode or control-flow prefix is empty"};
    }
    if (control_flow_record_count > maximum_control_flow_record_count ||
        control_flow_record_count > complete_ucode.size()) {
        return {{}, 0U, "Xenos shader control-flow prefix exceeds available records"};
    }

    const auto pair_count = static_cast<std::size_t>(control_flow_record_count);
    auto decoded = decode_shader_exec_pairs(complete_ucode.first(pair_count));
    if (!decoded) return {{}, 0U, decoded.error};

    for (std::size_t index = 0U; index < decoded.instructions.size(); ++index) {
        const auto& instruction = decoded.instructions[index];
        const auto opcode = instruction.opcode;
        const bool has_exec_payload = (opcode >= 1U && opcode <= 8U) ||
                                      opcode == 13U || opcode == 14U;
        if (!has_exec_payload) continue;

        const auto start = static_cast<std::size_t>(instruction.instruction_address);
        const auto count = static_cast<std::size_t>(instruction.instruction_count);
        if (start < pair_count || start > complete_ucode.size() ||
            count > complete_ucode.size() - start) {
            return {{}, 0U,
                    "Xenos EXEC block " + std::to_string(index) +
                        " addresses records outside the executable shader section"};
        }
    }

    return {std::move(decoded.instructions), control_flow_record_count, {}};
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
    ShaderVertexOutputs initial_vertex_outputs,
    std::span<const TextureFetchConstant> texture_constants) {
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

        const auto opcode = static_cast<std::uint8_t>(words[0] & 0x1fU);
        if (opcode == 0U) {
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
            continue;
        }

        const auto decoded = decode_texture_fetch_instruction(words);
        std::string texture_error;
        if (!execute_texture_fetch_instruction(decoded.instruction, texture_constants,
                                               memory, registers, texture_error)) {
            return {{}, 0U, false,
                    "Xenos EXEC fetch slot " + std::to_string(slot) + ": " + texture_error};
        }
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
    ShaderVertexOutputs initial_vertex_outputs,
    std::span<const TextureFetchConstant> texture_constants) {
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
            boolean_constants, vertex_outputs, texture_constants);
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
    std::span<const bool> boolean_constants,
    std::span<const TextureFetchConstant> texture_constants) {
    return execute_shader_exec_vertex_batch(
        std::span<const ShaderExecInstruction>(&exec, 1U), ucode, fetch_constants,
        memory, draw_vertex_indices, initial_registers, missing_component_values,
        float_constants, boolean_constants, texture_constants);
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
    std::span<const bool> boolean_constants,
    std::span<const TextureFetchConstant> texture_constants) {
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
                missing_component_values, float_constants, 0.0F, boolean_constants, {},
                texture_constants);
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
    std::array<bool, 32> required_texture_slots{};
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
                const auto fetch_opcode = static_cast<std::uint8_t>(words[0] & 0x1fU);
                if (fetch_opcode == 0U) {
                    const auto decoded = decode_vertex_fetch_instruction(words);
                    if (!decoded) {
                        return {{}, "Xenos EXEC fetch slot " + std::to_string(slot) + ": " +
                                        decoded.error};
                    }
                    required_fetch_slots[decoded.instruction.fetch_constant_index] = true;
                } else if (fetch_opcode == 1U) {
                    const auto decoded = decode_texture_fetch_instruction(words);
                    if (decoded.instruction.texture_slot >= required_texture_slots.size()) {
                        return {{}, "Xenos EXEC texture slot exceeds the 32-entry bank"};
                    }
                    required_texture_slots[decoded.instruction.texture_slot] = true;
                } else {
                    return {{}, "PM4 draw shader references an unsupported Xenos fetch opcode " +
                                    std::to_string(fetch_opcode)};
                }
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

    std::array<std::uint8_t, 32> texture_slots{};
    std::size_t texture_slot_count = 0U;
    for (std::size_t slot = 0U; slot < required_texture_slots.size(); ++slot) {
        if (required_texture_slots[slot]) {
            texture_slots[texture_slot_count++] = static_cast<std::uint8_t>(slot);
        }
    }
    const auto texture_bank = decode_texture_fetch_constant_bank(
        draw.register_snapshot,
        std::span<const std::uint8_t>(texture_slots.data(), texture_slot_count));
    if (!texture_bank) return {{}, "PM4 draw texture resources: " + texture_bank.error};

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
        initial_registers, missing_component_values, alu_bank.constants, boolean_constants,
        texture_bank.constants);
}

} // namespace project360::gpu::xenos
