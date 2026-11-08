#include "gpu/xenos/shader_alu.hpp"

#include <bit>
#include <cmath>

namespace project360::gpu::xenos {
namespace {

float multiply_xenos_legacy(float lhs, float rhs) noexcept {
    constexpr std::uint32_t exponent_mask = 0x7f800000U;
    if ((std::bit_cast<std::uint32_t>(lhs) & exponent_mask) == 0U ||
        (std::bit_cast<std::uint32_t>(rhs) & exponent_mask) == 0U) {
        return 0.0F;
    }
    return lhs * rhs;
}

} // namespace

ShaderAluInstruction decode_shader_alu_instruction(
    const std::array<std::uint32_t, 3>& words) noexcept {
    const auto word0 = words[0];
    const auto word1 = words[1];
    const auto word2 = words[2];

    ShaderAluInstruction instruction{};
    instruction.vector_destination = static_cast<std::uint8_t>(word0 & 0x3fU);
    instruction.vector_destination_relative = ((word0 >> 6U) & 1U) != 0U;
    instruction.absolute_constants = ((word0 >> 7U) & 1U) != 0U;
    instruction.scalar_destination = static_cast<std::uint8_t>((word0 >> 8U) & 0x3fU);
    instruction.scalar_destination_relative = ((word0 >> 14U) & 1U) != 0U;
    instruction.export_result = ((word0 >> 15U) & 1U) != 0U;
    instruction.vector_write_mask = static_cast<std::uint8_t>((word0 >> 16U) & 0x0fU);
    instruction.scalar_write_mask = static_cast<std::uint8_t>((word0 >> 20U) & 0x0fU);
    instruction.vector_clamp = ((word0 >> 24U) & 1U) != 0U;
    instruction.scalar_clamp = ((word0 >> 25U) & 1U) != 0U;
    instruction.scalar_opcode = static_cast<std::uint8_t>((word0 >> 26U) & 0x3fU);

    instruction.source_swizzles = {
        static_cast<std::uint8_t>((word1 >> 16U) & 0xffU),
        static_cast<std::uint8_t>((word1 >> 8U) & 0xffU),
        static_cast<std::uint8_t>(word1 & 0xffU),
    };
    instruction.source_negate = {
        ((word1 >> 26U) & 1U) != 0U,
        ((word1 >> 25U) & 1U) != 0U,
        ((word1 >> 24U) & 1U) != 0U,
    };
    instruction.predicate_condition = ((word1 >> 27U) & 1U) != 0U;
    instruction.predicated = ((word1 >> 28U) & 1U) != 0U;
    instruction.constant_address_register_relative = ((word1 >> 29U) & 1U) != 0U;
    instruction.constant_1_relative = ((word1 >> 30U) & 1U) != 0U;
    instruction.constant_0_relative = ((word1 >> 31U) & 1U) != 0U;

    instruction.source_registers = {
        static_cast<std::uint8_t>((word2 >> 16U) & 0xffU),
        static_cast<std::uint8_t>((word2 >> 8U) & 0xffU),
        static_cast<std::uint8_t>(word2 & 0xffU),
    };
    instruction.vector_opcode = static_cast<std::uint8_t>((word2 >> 24U) & 0x1fU);
    instruction.source_is_temporary = {
        ((word2 >> 31U) & 1U) != 0U,
        ((word2 >> 30U) & 1U) != 0U,
        ((word2 >> 29U) & 1U) != 0U,
    };
    return instruction;
}

ShaderAluSourceResult resolve_shader_alu_temporary_source(
    const ShaderAluInstruction& instruction, ShaderAluSource source,
    const ShaderAluTemporaryRegisterBank& temporary_registers) {
    std::size_t source_slot = 0;
    switch (source) {
    case ShaderAluSource::source_1:
        source_slot = 2U;
        break;
    case ShaderAluSource::source_2:
        source_slot = 1U;
        break;
    case ShaderAluSource::source_3:
        source_slot = 0U;
        break;
    default:
        return {{}, "Invalid Xenos ALU source selector"};
    }

    if (!instruction.source_is_temporary[source_slot]) {
        return {{}, "Xenos ALU constant sources are not implemented"};
    }

    const auto encoded_register = instruction.source_registers[source_slot];
    if ((encoded_register & 0x40U) != 0U) {
        return {{}, "Loop-relative Xenos temporary sources are not implemented"};
    }
    const auto register_index = static_cast<std::size_t>(encoded_register & 0x3fU);
    const auto swizzle = instruction.source_swizzles[source_slot];
    ShaderAluSourceResult result{};
    for (std::size_t lane = 0; lane < result.value.size(); ++lane) {
        const auto component = (swizzle >> (2U * lane)) & 0x03U;
        const auto source_component = (component + lane) & 0x03U;
        auto value = temporary_registers[register_index][source_component];
        if ((encoded_register & 0x80U) != 0U) {
            value = std::abs(value);
        }
        if (instruction.source_negate[source_slot]) {
            value = -value;
        }
        result.value[lane] = value;
    }
    return result;
}

ShaderVectorAluResult evaluate_shader_vector_alu(
    const ShaderAluInstruction& instruction, const std::array<float, 4>& source_1,
    const std::array<float, 4>& source_2,
    const std::array<float, 4>& previous_destination) {
    if (instruction.predicated) {
        return {{}, "Predicated Xenos vector ALU execution is not implemented"};
    }
    if (instruction.vector_destination_relative) {
        return {{}, "Relative Xenos vector destinations are not implemented"};
    }
    if (instruction.export_result) {
        return {{}, "Xenos ALU export writes are not implemented"};
    }
    if (instruction.vector_clamp) {
        return {{}, "Clamped Xenos vector ALU results are not implemented"};
    }
    if (instruction.vector_opcode > 7U || instruction.vector_opcode == 2U ||
        instruction.vector_opcode == 3U) {
        return {{}, "Unsupported Xenos vector ALU opcode"};
    }

    ShaderVectorAluResult result{previous_destination, {}};
    for (std::size_t lane = 0; lane < result.destination.size(); ++lane) {
        if ((instruction.vector_write_mask & (1U << lane)) == 0U) {
            continue;
        }
        switch (instruction.vector_opcode) {
        case 0U:
            result.destination[lane] = source_1[lane] + source_2[lane];
            break;
        case 1U:
            result.destination[lane] = multiply_xenos_legacy(source_1[lane], source_2[lane]);
            break;
        case 4U:
            result.destination[lane] = source_1[lane] == source_2[lane] ? 1.0F : 0.0F;
            break;
        case 5U:
            result.destination[lane] = source_1[lane] > source_2[lane] ? 1.0F : 0.0F;
            break;
        case 6U:
            result.destination[lane] = source_1[lane] >= source_2[lane] ? 1.0F : 0.0F;
            break;
        case 7U:
            result.destination[lane] = source_1[lane] != source_2[lane] ? 1.0F : 0.0F;
            break;
        default:
            return {{}, "Unsupported Xenos vector ALU opcode"};
        }
    }
    return result;
}

ShaderVectorAluResult evaluate_shader_vector_alu_from_temporaries(
    const ShaderAluInstruction& instruction,
    const ShaderAluTemporaryRegisterBank& temporary_registers,
    const std::array<float, 4>& previous_destination) {
    const auto source_1 = resolve_shader_alu_temporary_source(
        instruction, ShaderAluSource::source_1, temporary_registers);
    if (!source_1) {
        return {{}, source_1.error};
    }
    const auto source_2 = resolve_shader_alu_temporary_source(
        instruction, ShaderAluSource::source_2, temporary_registers);
    if (!source_2) {
        return {{}, source_2.error};
    }
    return evaluate_shader_vector_alu(instruction, source_1.value, source_2.value,
                                      previous_destination);
}

ShaderAluExecutionResult execute_shader_alu_add_mul_pair(
    const ShaderAluInstruction& instruction,
    const ShaderAluTemporaryRegisterBank& initial_registers) {
    if (instruction.predicated) {
        return {{}, 0.0F, "Predicated Xenos ALU execution is not implemented"};
    }
    if (instruction.vector_destination_relative || instruction.scalar_destination_relative) {
        return {{}, 0.0F, "Relative Xenos ALU destinations are not implemented"};
    }
    if (instruction.export_result) {
        return {{}, 0.0F, "Xenos ALU export writes are not implemented"};
    }
    if (instruction.vector_clamp || instruction.scalar_clamp) {
        return {{}, 0.0F, "Clamped Xenos ALU results are not implemented"};
    }
    if (instruction.vector_opcode > 1U ||
        (instruction.scalar_opcode != 0U && instruction.scalar_opcode != 2U)) {
        return {{}, 0.0F, "Unsupported Xenos vector/scalar ALU opcode pair"};
    }

    const auto source_1 = resolve_shader_alu_temporary_source(
        instruction, ShaderAluSource::source_1, initial_registers);
    if (!source_1) {
        return {{}, 0.0F, source_1.error};
    }
    const auto source_2 = resolve_shader_alu_temporary_source(
        instruction, ShaderAluSource::source_2, initial_registers);
    if (!source_2) {
        return {{}, 0.0F, source_2.error};
    }
    const auto scalar_source = resolve_shader_alu_temporary_source(
        instruction, ShaderAluSource::source_3, initial_registers);
    if (!scalar_source) {
        return {{}, 0.0F, scalar_source.error};
    }
    const auto vector_result = evaluate_shader_vector_alu(
        instruction, source_1.value, source_2.value,
        initial_registers[instruction.vector_destination]);
    if (!vector_result) {
        return {{}, 0.0F, vector_result.error};
    }

    if (instruction.vector_destination == instruction.scalar_destination &&
        (instruction.vector_write_mask & instruction.scalar_write_mask) != 0U) {
        return {{}, 0.0F, "Overlapping vector/scalar writes to one Xenos register are unsupported"};
    }

    // ADDS/MULS consume the W (`a`) and X (`b`) swizzles of src3.
    const auto scalar_result = instruction.scalar_opcode == 0U
                                   ? scalar_source.value[3] + scalar_source.value[0]
                                   : multiply_xenos_legacy(scalar_source.value[3],
                                                           scalar_source.value[0]);
    ShaderAluExecutionResult result{initial_registers, scalar_result, {}};
    result.registers[instruction.vector_destination] = vector_result.destination;
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if ((instruction.scalar_write_mask & (1U << lane)) != 0U) {
            result.registers[instruction.scalar_destination][lane] = scalar_result;
        }
    }
    return result;
}

} // namespace project360::gpu::xenos
