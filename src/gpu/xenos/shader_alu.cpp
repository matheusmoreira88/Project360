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

float select_xenos_operand(float lhs, float rhs, bool select_lhs) noexcept {
    const auto lhs_bits = std::bit_cast<std::uint32_t>(lhs);
    const auto rhs_bits = std::bit_cast<std::uint32_t>(rhs);
    return std::bit_cast<float>(select_lhs ? lhs_bits : rhs_bits);
}

bool is_supported_vector_opcode(std::uint8_t opcode) noexcept {
    return opcode <= 7U || opcode == 15U || opcode == 16U || opcode == 17U;
}

float dot_product_xenos(const std::array<float, 4>& lhs, const std::array<float, 4>& rhs,
                        std::size_t component_count) noexcept {
    // Preserve each FP32 product and sum separately; host FMA contraction would
    // change the guest result for cancellation-sensitive inputs.
    volatile float product_x = multiply_xenos_legacy(lhs[0], rhs[0]);
    volatile float product_y = multiply_xenos_legacy(lhs[1], rhs[1]);
    volatile float product_z = multiply_xenos_legacy(lhs[2], rhs[2]);
    volatile float sum_xy = product_x + product_y;
    volatile float sum_xyz = sum_xy + product_z;
    if (component_count == 3U) {
        return sum_xyz;
    }

    volatile float product_w = multiply_xenos_legacy(lhs[3], rhs[3]);
    volatile float sum_xyzw = sum_xyz + product_w;
    return sum_xyzw;
}

float dot2add_xenos(const std::array<float, 4>& lhs, const std::array<float, 4>& rhs,
                    const std::array<float, 4>& addend) noexcept {
    // DP2ADD uses only src3.x; products and additions round separately.
    volatile float product_x = multiply_xenos_legacy(lhs[0], rhs[0]);
    volatile float product_y = multiply_xenos_legacy(lhs[1], rhs[1]);
    volatile float sum_xy = product_x + product_y;
    volatile float result = sum_xy + addend[0];
    return result;
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
    instruction.constant_absolute_value = ((word0 >> 7U) & 1U) != 0U;
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

ShaderAluSourceResult resolve_shader_alu_source(
    const ShaderAluInstruction& instruction, ShaderAluSource source,
    const ShaderAluTemporaryRegisterBank& temporary_registers,
    std::span<const std::array<float, 4>> float_constants) {
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

    const auto encoded_register = instruction.source_registers[source_slot];
    const bool is_temporary = instruction.source_is_temporary[source_slot];
    std::size_t register_index = encoded_register;
    if (is_temporary) {
        if ((encoded_register & 0x40U) != 0U) {
            return {{}, "Loop-relative Xenos temporary sources are not implemented"};
        }
        register_index = static_cast<std::size_t>(encoded_register & 0x3fU);
    } else {
        bool constant_is_relative = false;
        if (source_slot == 2U) {
            constant_is_relative = instruction.constant_0_relative;
        } else if (source_slot == 1U) {
            constant_is_relative = instruction.source_is_temporary[2U]
                                       ? instruction.constant_0_relative
                                       : instruction.constant_1_relative;
        } else {
            constant_is_relative =
                instruction.source_is_temporary[2U] &&
                        instruction.source_is_temporary[1U]
                    ? instruction.constant_0_relative
                    : instruction.constant_1_relative;
        }
        if (constant_is_relative) {
            return {{}, instruction.constant_address_register_relative
                            ? "Address-register-relative Xenos float constants are not implemented"
                            : "Loop-relative Xenos float constants are not implemented"};
        }
        if (register_index >= float_constants.size()) {
            return {{}, "Xenos float-constant index is outside the supplied constant bank"};
        }
    }

    const auto swizzle = instruction.source_swizzles[source_slot];
    ShaderAluSourceResult result{};
    for (std::size_t lane = 0; lane < result.value.size(); ++lane) {
        const auto component = (swizzle >> (2U * lane)) & 0x03U;
        const auto source_component = (component + lane) & 0x03U;
        auto value = is_temporary ? temporary_registers[register_index][source_component]
                                  : float_constants[register_index][source_component];
        if (is_temporary ? ((encoded_register & 0x80U) != 0U)
                         : instruction.constant_absolute_value) {
            value = std::abs(value);
        }
        if (instruction.source_negate[source_slot]) {
            value = -value;
        }
        result.value[lane] = value;
    }
    return result;
}

ShaderAluSourceResult resolve_shader_alu_temporary_source(
    const ShaderAluInstruction& instruction, ShaderAluSource source,
    const ShaderAluTemporaryRegisterBank& temporary_registers) {
    return resolve_shader_alu_source(instruction, source, temporary_registers, {});
}

ShaderVectorAluResult evaluate_shader_vector_alu(const ShaderAluInstruction& instruction,
                                                 const std::array<float, 4>& source_1,
                                                 const std::array<float, 4>& source_2,
                                                 const std::array<float, 4>& previous_destination,
                                                 const std::array<float, 4>& source_3) {
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
    if (!is_supported_vector_opcode(instruction.vector_opcode)) {
        return {{}, "Unsupported Xenos vector ALU opcode"};
    }

    if (instruction.vector_opcode == 2U || instruction.vector_opcode == 3U) {
        for (std::size_t lane = 0; lane < 4U; ++lane) {
            if ((instruction.vector_write_mask & (1U << lane)) != 0U &&
                (std::isnan(source_1[lane]) || std::isnan(source_2[lane]))) {
                return {{}, "Unordered Xenos MAX/MIN operands are not validated"};
            }
        }
    }

    const float dot_result =
        instruction.vector_opcode == 15U   ? dot_product_xenos(source_1, source_2, 4U)
        : instruction.vector_opcode == 16U ? dot_product_xenos(source_1, source_2, 3U)
                                           : 0.0F;
    const float dot2add_result =
        instruction.vector_opcode == 17U ? dot2add_xenos(source_1, source_2, source_3) : 0.0F;
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
        case 2U:
            result.destination[lane] = select_xenos_operand(source_1[lane], source_2[lane],
                                                            source_1[lane] >= source_2[lane]);
            break;
        case 3U:
            result.destination[lane] = select_xenos_operand(source_1[lane], source_2[lane],
                                                            source_1[lane] < source_2[lane]);
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
        case 15U:
        case 16U:
            result.destination[lane] = dot_result;
            break;
        case 17U:
            result.destination[lane] = dot2add_result;
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
    const std::array<float, 4>& previous_destination,
    std::span<const std::array<float, 4>> float_constants) {
    const auto source_1 = resolve_shader_alu_source(
        instruction, ShaderAluSource::source_1, temporary_registers, float_constants);
    if (!source_1) {
        return {{}, source_1.error};
    }
    const auto source_2 = resolve_shader_alu_source(
        instruction, ShaderAluSource::source_2, temporary_registers, float_constants);
    if (!source_2) {
        return {{}, source_2.error};
    }
    std::array<float, 4> source_3{};
    if (instruction.vector_opcode == 17U) {
        const auto resolved_source_3 = resolve_shader_alu_source(
            instruction, ShaderAluSource::source_3, temporary_registers, float_constants);
        if (!resolved_source_3) {
            return {{}, resolved_source_3.error};
        }
        source_3 = resolved_source_3.value;
    }
    return evaluate_shader_vector_alu(instruction, source_1.value, source_2.value,
                                      previous_destination, source_3);
}

ShaderAluExecutionResult
execute_shader_alu_add_mul_pair(const ShaderAluInstruction& instruction,
                                const ShaderAluTemporaryRegisterBank& initial_registers,
                                std::span<const std::array<float, 4>> float_constants,
                                float initial_previous_scalar) {
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
    if (!is_supported_vector_opcode(instruction.vector_opcode) || instruction.scalar_opcode > 3U) {
        return {{}, 0.0F, "Unsupported Xenos vector/scalar ALU opcode pair"};
    }

    const auto source_1 = resolve_shader_alu_source(instruction, ShaderAluSource::source_1,
                                                    initial_registers, float_constants);
    if (!source_1) {
        return {{}, 0.0F, source_1.error};
    }
    const auto source_2 = resolve_shader_alu_source(instruction, ShaderAluSource::source_2,
                                                    initial_registers, float_constants);
    if (!source_2) {
        return {{}, 0.0F, source_2.error};
    }
    const auto scalar_source = resolve_shader_alu_source(instruction, ShaderAluSource::source_3,
                                                         initial_registers, float_constants);
    if (!scalar_source) {
        return {{}, 0.0F, scalar_source.error};
    }
    const auto vector_result = evaluate_shader_vector_alu(
        instruction, source_1.value, source_2.value,
        initial_registers[instruction.vector_destination], scalar_source.value);
    if (!vector_result) {
        return {{}, 0.0F, vector_result.error};
    }

    if (instruction.vector_destination == instruction.scalar_destination &&
        (instruction.vector_write_mask & instruction.scalar_write_mask) != 0U) {
        return {{}, 0.0F, "Overlapping vector/scalar writes to one Xenos register are unsupported"};
    }

    // Scalar operations use the W (`a`) and X (`b`) swizzles of src3.
    float scalar_result = 0.0F;
    switch (instruction.scalar_opcode) {
    case 0U: // ADDS: a + b
        scalar_result = scalar_source.value[3] + scalar_source.value[0];
        break;
    case 1U: // ADDS_PREV: a + ps
        scalar_result = scalar_source.value[3] + initial_previous_scalar;
        break;
    case 2U: // MULS: a * b
        scalar_result = multiply_xenos_legacy(scalar_source.value[3], scalar_source.value[0]);
        break;
    case 3U: // MULS_PREV: a * ps
        scalar_result = multiply_xenos_legacy(scalar_source.value[3], initial_previous_scalar);
        break;
    default:
        return {{}, 0.0F, "Unsupported Xenos scalar ALU opcode"};
    }
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
