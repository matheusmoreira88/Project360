#include "gpu/xenos/shader_alu.hpp"

#include <cmath>

namespace project360::gpu::xenos {

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
    if (instruction.vector_opcode > 1U) {
        return {{}, "Unsupported Xenos vector ALU opcode"};
    }

    ShaderVectorAluResult result{previous_destination, {}};
    for (std::size_t lane = 0; lane < result.destination.size(); ++lane) {
        if ((instruction.vector_write_mask & (1U << lane)) == 0U) {
            continue;
        }
        if (instruction.vector_opcode == 0U) {
            result.destination[lane] = source_1[lane] + source_2[lane];
        } else {
            result.destination[lane] = source_1[lane] * source_2[lane];
        }
    }
    return result;
}

} // namespace project360::gpu::xenos
