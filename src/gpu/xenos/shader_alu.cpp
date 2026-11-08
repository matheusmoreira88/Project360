#include "gpu/xenos/shader_alu.hpp"

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

} // namespace project360::gpu::xenos
