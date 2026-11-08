#include "gpu/xenos/pm4_decoder.hpp"
#include "gpu/xenos/pm4_draw.hpp"
#include "gpu/xenos/pm4_registers.hpp"
#include "gpu/xenos/raster_state.hpp"
#include "gpu/xenos/registers.hpp"
#include "gpu/xenos/shader_alu.hpp"
#include "gpu/xenos/shader_execution.hpp"
#include "gpu/xenos/texture_decode.hpp"
#include "gpu/xenos/texture_fetch.hpp"
#include "gpu/xenos/texture_memory.hpp"
#include "gpu/xenos/vertex_fetch.hpp"
#include "memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void append_u32_be(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void test_packet_framing_and_metadata() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> stream;
    append_u32_be(stream, (1U << 16U) | 0x0123U); // Type 0, two sequential registers.
    append_u32_be(stream, 0x11111111U);
    append_u32_be(stream, 0x22222222U);
    append_u32_be(stream, 0x40000000U | (0x345U << 11U) | 0x123U); // Type 1.
    append_u32_be(stream, 0x33333333U);
    append_u32_be(stream, 0x44444444U);
    append_u32_be(stream, 0x80000000U); // Type 2 no-op.
    append_u32_be(stream, 0xc0000000U | (2U << 16U) | (0x2dU << 8U) | 1U); // Type 3.
    append_u32_be(stream, 0x55555555U);
    append_u32_be(stream, 0x66666666U);
    append_u32_be(stream, 0x77777777U);

    const auto decoded = decode_pm4(stream);
    check(static_cast<bool>(decoded) && decoded.packets.size() == 4U,
          "mixed PM4 stream decodes into four packets");
    if (!decoded || decoded.packets.size() != 4U) return;

    const auto& type0 = decoded.packets[0];
    check(type0.type == PacketType::type0_registers && type0.register_index == 0x123U &&
              type0.payload_word_count == 2U && type0.payload_word_offset == 1U &&
              !type0.write_one_register,
          "type-0 header exposes base register, sequential mode and bounded payload");
    const auto& type1 = decoded.packets[1];
    check(type1.type == PacketType::type1_registers && type1.register_index == 0x123U &&
              type1.second_register_index == 0x345U && type1.payload_word_count == 2U,
          "type-1 header exposes both register indexes and its two-word payload");
    check(decoded.packets[2].type == PacketType::type2_nop &&
              decoded.packets[2].payload_word_count == 0U,
          "type-2 packet consumes only its header");
    const auto& type3 = decoded.packets[3];
    check(type3.type == PacketType::type3_command && type3.opcode == 0x2dU &&
              type3.predicated && type3.payload_word_count == 3U &&
              type3.payload_word_offset == 8U,
          "type-3 header exposes opcode, predicate bit and payload bounds");
}

void test_shader_alu_word_decode() {
    using namespace project360::gpu::xenos;
    constexpr std::array<std::uint32_t, 3> words{
        0xabebefd2U,
        0xffdcba98U,
        0xd6543210U,
    };
    const auto instruction = decode_shader_alu_instruction(words);
    check(instruction.vector_destination == 0x12U &&
              instruction.vector_destination_relative && instruction.constant_absolute_value &&
              instruction.scalar_destination == 0x2fU &&
              instruction.scalar_destination_relative && instruction.export_result &&
              instruction.vector_write_mask == 0x0bU && instruction.scalar_write_mask == 0x0eU &&
              instruction.vector_clamp && instruction.scalar_clamp &&
              instruction.scalar_opcode == 0x2aU,
          "Xenos ALU word 0 decodes destinations, masks, export and scalar opcode fields");
    check(instruction.source_swizzles == std::array<std::uint8_t, 3>{0xdcU, 0xbaU, 0x98U} &&
              instruction.source_negate == std::array<bool, 3>{true, true, true} &&
              instruction.predicate_condition && instruction.predicated &&
              instruction.constant_address_register_relative &&
              instruction.constant_1_relative && instruction.constant_0_relative,
          "Xenos ALU word 1 decodes swizzles, negation, predicate and constant addressing");
    check(instruction.source_registers == std::array<std::uint8_t, 3>{0x54U, 0x32U, 0x10U} &&
              instruction.vector_opcode == 0x16U &&
              instruction.source_is_temporary == std::array<bool, 3>{true, true, false},
          "Xenos ALU word 2 decodes raw source registers, selectors and vector opcode");

    constexpr std::array<std::uint32_t, 3> unknown_opcodes{
        (0x3fU << 26U), (0U), (0x1fU << 24U),
    };
    const auto unknown = decode_shader_alu_instruction(unknown_opcodes);
    check(unknown.scalar_opcode == 0x3fU && unknown.vector_opcode == 0x1fU,
          "Xenos ALU decoder preserves unknown opcode values for explicit later rejection");
}

void test_shader_vector_alu_reference_subset() {
    using namespace project360::gpu::xenos;
    const std::array<float, 4> source_1{1.0F, -2.0F, 3.5F, 4.0F};
    const std::array<float, 4> source_2{2.0F, 5.0F, -0.5F, 8.0F};
    const std::array<float, 4> previous{9.0F, 9.0F, 9.0F, 9.0F};

    const auto add = decode_shader_alu_instruction({0x000b0000U, 0U, 0U});
    const auto add_result = evaluate_shader_vector_alu(add, source_1, source_2, previous);
    check(add_result && add_result.destination == std::array<float, 4>{3.0F, 3.0F, 9.0F, 12.0F},
          "Xenos vector ADD evaluates enabled lanes and preserves masked destination lanes");

    const auto multiply = decode_shader_alu_instruction({0x000f0000U, 0U, 0x01000000U});
    const auto multiply_result = evaluate_shader_vector_alu(
        multiply, source_1, source_2, previous);
    check(multiply_result &&
              multiply_result.destination == std::array<float, 4>{2.0F, -10.0F, -1.75F, 32.0F},
          "Xenos vector MUL evaluates all four lanes from decoded opcode fields");

    auto unsupported = add;
    unsupported.vector_opcode = 11U;
    const auto unsupported_result = evaluate_shader_vector_alu(
        unsupported, source_1, source_2, previous);
    auto predicated = add;
    predicated.predicated = true;
    const auto predicated_result = evaluate_shader_vector_alu(
        predicated, source_1, source_2, previous);
    check(!unsupported_result && unsupported_result.destination == std::array<float, 4>{} &&
              !predicated_result && predicated_result.destination == std::array<float, 4>{},
          "Xenos vector ALU subset rejects unsupported opcodes and predicates without partial output");
}

void test_shader_vector_alu_comparisons() {
    using namespace project360::gpu::xenos;
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const std::array<float, 4> source_1{-1.0F, 2.0F, 3.0F, nan};
    const std::array<float, 4> source_2{-1.0F, 3.0F, 2.0F, nan};
    const std::array<float, 4> previous{9.0F, 9.0F, 9.0F, 9.0F};
    const auto evaluate = [&](std::uint8_t opcode) {
        const auto instruction = decode_shader_alu_instruction(
            {0x000f0000U, 0U, static_cast<std::uint32_t>(opcode) << 24U});
        return evaluate_shader_vector_alu(instruction, source_1, source_2, previous);
    };

    const auto equal = evaluate(4U);
    const auto greater = evaluate(5U);
    const auto greater_equal = evaluate(6U);
    const auto not_equal = evaluate(7U);
    check(equal && equal.destination == std::array<float, 4>{1.0F, 0.0F, 0.0F, 0.0F} &&
              greater && greater.destination == std::array<float, 4>{0.0F, 0.0F, 1.0F, 0.0F} &&
              greater_equal &&
                  greater_equal.destination == std::array<float, 4>{1.0F, 0.0F, 1.0F, 0.0F} &&
              not_equal && not_equal.destination == std::array<float, 4>{0.0F, 1.0F, 1.0F, 1.0F},
          "Xenos SEQ/SGT/SGE/SNE return per-lane booleans with IEEE unordered comparisons");

    const std::array<float, 4> ordered_left{-2.0F, 4.0F, -0.0F,
                                            std::numeric_limits<float>::infinity()};
    const std::array<float, 4> ordered_right{-3.0F, -1.0F, 0.0F, 10.0F};
    const auto maximum =
        evaluate_shader_vector_alu(decode_shader_alu_instruction({0x000f0000U, 0U, 0x02000000U}),
                                   ordered_left, ordered_right, previous);
    const auto minimum =
        evaluate_shader_vector_alu(decode_shader_alu_instruction({0x000f0000U, 0U, 0x03000000U}),
                                   ordered_left, ordered_right, previous);
    check(maximum && maximum.destination[0] == -2.0F && maximum.destination[1] == 4.0F &&
              std::isinf(maximum.destination[3]) && minimum &&
              minimum.destination == std::array<float, 4>{-3.0F, -1.0F, 0.0F, 10.0F},
          "Xenos MAX/MIN use documented per-lane compare-select rules for finite values and "
          "infinities");
    check(std::signbit(maximum.destination[2]) && !std::signbit(minimum.destination[2]),
          "Xenos MAX/MIN preserve the source selected by their signed-zero tie rules");

    const std::array<float, 4> condition{-0.0F, -1.0F, 1.0F, nan};
    const std::array<float, 4> conditional_source_1{10.0F, 20.0F, 30.0F, 40.0F};
    const std::array<float, 4> conditional_source_2{100.0F, 200.0F, 300.0F, 400.0F};
    const auto evaluate_conditional = [&](std::uint8_t opcode) {
        const auto instruction = decode_shader_alu_instruction(
            {0x000f0000U, 0U, static_cast<std::uint32_t>(opcode) << 24U});
        return evaluate_shader_vector_alu(instruction, condition, conditional_source_1, previous,
                                          conditional_source_2);
    };
    const auto condition_equal_zero = evaluate_conditional(12U);
    const auto condition_greater_equal_zero = evaluate_conditional(13U);
    const auto condition_greater_zero = evaluate_conditional(14U);
    check(condition_equal_zero &&
              condition_equal_zero.destination ==
                  std::array<float, 4>{10.0F, 200.0F, 300.0F, 400.0F} &&
              condition_greater_equal_zero &&
              condition_greater_equal_zero.destination ==
                  std::array<float, 4>{10.0F, 200.0F, 30.0F, 400.0F} &&
              condition_greater_zero &&
              condition_greater_zero.destination ==
                  std::array<float, 4>{100.0F, 200.0F, 30.0F, 400.0F},
          "Xenos CNDE/CNDGE/CNDGT select source lanes from zero comparisons, including NaN");

    auto unordered_right = ordered_right;
    unordered_right[1] = std::numeric_limits<float>::quiet_NaN();
    const auto unordered =
        evaluate_shader_vector_alu(decode_shader_alu_instruction({0x000f0000U, 0U, 0x02000000U}),
                                   ordered_left, unordered_right, previous);
    check(!unordered && unordered.destination == std::array<float, 4>{},
          "Xenos MAX/MIN reject unordered operands until Xenos NaN behavior is verified");
}

void test_shader_vector_alu_rounding() {
    using namespace project360::gpu::xenos;
    const std::array<float, 4> source{-1.25F, 1.25F, -0.0F, std::numeric_limits<float>::infinity()};
    const std::array<float, 4> other{};
    const std::array<float, 4> previous{};
    const auto evaluate = [&](std::uint8_t opcode) {
        const auto instruction = decode_shader_alu_instruction(
            {0x000f0000U, 0U, static_cast<std::uint32_t>(opcode) << 24U});
        return evaluate_shader_vector_alu(instruction, source, other, previous);
    };

    const auto fraction = evaluate(8U);
    const auto truncated = evaluate(9U);
    const auto floored = evaluate(10U);
    check(fraction && fraction.destination[0] == 0.75F && fraction.destination[1] == 0.25F &&
              fraction.destination[2] == 0.0F && !std::signbit(fraction.destination[2]) &&
              std::isnan(fraction.destination[3]) && truncated &&
              truncated.destination[0] == -1.0F && truncated.destination[1] == 1.0F &&
              truncated.destination[2] == 0.0F && std::signbit(truncated.destination[2]) &&
              std::isinf(truncated.destination[3]) && floored && floored.destination[0] == -2.0F &&
              floored.destination[1] == 1.0F && floored.destination[2] == 0.0F &&
              std::signbit(floored.destination[2]) && std::isinf(floored.destination[3]),
          "Xenos FRC/TRUNC/FLOOR handle negative fractions, signed zero and infinity");
}

void test_shader_vector_alu_dot_products() {
    using namespace project360::gpu::xenos;
    const std::array<float, 4> previous{9.0F, 9.0F, 9.0F, 9.0F};
    const std::array<float, 4> source_1{1.0F, 2.0F, 3.0F, 1000.0F};
    const std::array<float, 4> source_2{4.0F, 5.0F, 6.0F, -1000.0F};
    const auto dot3 =
        evaluate_shader_vector_alu(decode_shader_alu_instruction({0x00050000U, 0U, 0x10000000U}),
                                   source_1, source_2, previous);
    const auto dot4 =
        evaluate_shader_vector_alu(decode_shader_alu_instruction({0x000f0000U, 0U, 0x0f000000U}),
                                   {1.0F, 2.0F, 3.0F, 4.0F}, {2.0F, 3.0F, 4.0F, 5.0F}, previous);
    check(dot3 && dot3.destination == std::array<float, 4>{32.0F, 9.0F, 32.0F, 9.0F} && dot4 &&
              dot4.destination == std::array<float, 4>{40.0F, 40.0F, 40.0F, 40.0F},
          "Xenos DP3 ignores W and replicates its result; DP4 includes and replicates W");

    const auto ordered_dot4 = evaluate_shader_vector_alu(
        decode_shader_alu_instruction({0x000f0000U, 0U, 0x0f000000U}),
        {1.0e20F, -1.0e20F, 3.0F, 4.0F}, {1.0F, 1.0F, 1.0F, 1.0F}, previous);
    check(ordered_dot4 && ordered_dot4.destination == std::array<float, 4>{7.0F, 7.0F, 7.0F, 7.0F},
          "Xenos DP4 follows the documented XYZW accumulation order");

    const auto legacy_dot3 = evaluate_shader_vector_alu(
        decode_shader_alu_instruction({0x000f0000U, 0U, 0x10000000U}),
        {0.0F, std::numeric_limits<float>::denorm_min(), 2.0F, 100.0F},
        {std::numeric_limits<float>::infinity(), 2.0F, 3.0F, 100.0F}, previous);
    check(legacy_dot3 && legacy_dot3.destination == std::array<float, 4>{6.0F, 6.0F, 6.0F, 6.0F},
          "Xenos DP3 uses the established legacy zero/subnormal product behavior");

    constexpr float rounded_product_operand = 1.00000011920928955078125F;
    constexpr float rounded_product_cancel = 1.0000002384185791015625F;
    const auto cancellation_dot3 = evaluate_shader_vector_alu(
        decode_shader_alu_instruction({0x000f0000U, 0U, 0x10000000U}),
        {rounded_product_operand, -1.0F, 0.0F, 0.0F},
        {rounded_product_operand, rounded_product_cancel, 0.0F, 0.0F}, previous);
    check(cancellation_dot3 &&
              cancellation_dot3.destination == std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F},
          "Xenos DP3 rounds products before accumulation instead of fusing host multiply-adds");

    ShaderAluTemporaryRegisterBank registers{};
    ShaderAluFloatConstantBank constants{};
    registers[1] = {2.0F, 3.0F, 50.0F, 100.0F};
    registers[2] = {4.0F, 5.0F, 60.0F, 200.0F};
    constants[7] = {6.0F, 1000.0F, -1000.0F, 50.0F};
    const auto encoded_dp2add = decode_shader_alu_instruction({0x000f0004U, 0U, 0x71070201U});
    const auto dp2add =
        evaluate_shader_vector_alu_from_temporaries(encoded_dp2add, registers, previous, constants);
    check(dp2add && dp2add.destination == std::array<float, 4>{29.0F, 29.0F, 29.0F, 29.0F},
          "Xenos DP2ADD resolves src3 from an absolute float constant and uses only src3.x");
}

void test_shader_vector_alu_legacy_multiplication() {
    using namespace project360::gpu::xenos;
    const auto instruction = decode_shader_alu_instruction({0x000f0000U, 0U, 0x01000000U});
    const std::array<float, 4> source_1{
        0.0F, -0.0F, std::numeric_limits<float>::denorm_min(), 2.0F};
    const std::array<float, 4> source_2{
        -0.0F, 3.0F, std::numeric_limits<float>::infinity(), 4.0F};
    const auto result = evaluate_shader_vector_alu(
        instruction, source_1, source_2, {9.0F, 9.0F, 9.0F, 9.0F});
    check(result && result.destination == std::array<float, 4>{0.0F, 0.0F, 0.0F, 8.0F} &&
              !std::signbit(result.destination[0]) && !std::signbit(result.destination[1]) &&
              !std::signbit(result.destination[2]),
          "Xenos legacy MUL flushes zero/subnormal operands to positive zero per lane");
}

void test_shader_alu_temporary_source_resolution() {
    using namespace project360::gpu::xenos;
    ShaderAluTemporaryRegisterBank registers{};
    registers[5] = {-10.0F, 20.0F, -30.0F, 40.0F};

    // src1 is the low register byte, low swizzle byte and bit 29 selector.
    const auto instruction = decode_shader_alu_instruction(
        {0U, 0x01000055U, 0x20000000U | 0x00000085U});
    const auto source = resolve_shader_alu_temporary_source(
        instruction, ShaderAluSource::source_1, registers);
    check(source && source.value == std::array<float, 4>{-20.0F, -30.0F, -40.0F, -10.0F},
          "Xenos temporary source resolution applies relative swizzle, absolute value and negate");

    const auto constant_instruction = decode_shader_alu_instruction(
        {0U, 0U, 0x00000005U});
    const auto constant_source = resolve_shader_alu_temporary_source(
        constant_instruction, ShaderAluSource::source_1, registers);
    auto relative_words = std::array<std::uint32_t, 3>{
        0U, 0U, 0x20000000U | 0x00000045U};
    const auto relative_instruction = decode_shader_alu_instruction(relative_words);
    const auto relative_source = resolve_shader_alu_temporary_source(
        relative_instruction, ShaderAluSource::source_1, registers);
    check(!constant_source && constant_source.value == std::array<float, 4>{} &&
              !relative_source && relative_source.value == std::array<float, 4>{},
          "Xenos source resolver rejects constants and loop-relative registers without partial values");
}

void test_shader_alu_absolute_float_constant_sources() {
    using namespace project360::gpu::xenos;
    ShaderAluTemporaryRegisterBank registers{};
    ShaderAluFloatConstantBank constants{};
    constants[5] = {-1.0F, 2.0F, -3.0F, 4.0F};

    // src1 uses constant c5; abs_constants and src1 negate are both set.
    const auto instruction = decode_shader_alu_instruction(
        {0x00000080U, 0x01000000U, (1U << 30U) | (2U << 8U) | 5U});
    const auto source = resolve_shader_alu_source(
        instruction, ShaderAluSource::source_1, registers, constants);
    check(source && source.value == std::array<float, 4>{-1.0F, -2.0F, -3.0F, -4.0F},
          "absolute Xenos float constants apply swizzle, abs_constants and operand negate");

    const auto relative_instruction = decode_shader_alu_instruction(
        {0U, 0x80000000U, (1U << 30U) | (2U << 8U) | 5U});
    const auto relative = resolve_shader_alu_source(
        relative_instruction, ShaderAluSource::source_1, registers, constants);
    const auto address_relative_instruction = decode_shader_alu_instruction(
        {0U, 0xa0000000U, (1U << 30U) | (2U << 8U) | 5U});
    const auto address_relative = resolve_shader_alu_source(
        address_relative_instruction, ShaderAluSource::source_1, registers, constants);
    const auto missing = resolve_shader_alu_source(
        instruction, ShaderAluSource::source_1, registers, {});
    constants[6] = {10.0F, 20.0F, 30.0F, 40.0F};
    const auto second_constant_instruction = decode_shader_alu_instruction(
        {0U, 0U, (6U << 8U) | 5U});
    const auto second_constant = resolve_shader_alu_source(
        second_constant_instruction, ShaderAluSource::source_2, registers, constants);
    check(!relative && !address_relative && !missing &&
              relative.value == std::array<float, 4>{} &&
              address_relative.value == std::array<float, 4>{} &&
              missing.value == std::array<float, 4>{} && second_constant &&
              second_constant.value == constants[6],
          "Xenos source resolver rejects unavailable relative constants and selects src2 constants");

    constants[5] = {1.0F, 2.0F, 3.0F, 4.0F};
    ShaderAluInstruction scalar_constant_instruction{};
    scalar_constant_instruction.vector_destination = 4U;
    scalar_constant_instruction.scalar_destination = 5U;
    scalar_constant_instruction.scalar_write_mask = 0x0fU;
    scalar_constant_instruction.scalar_opcode = 0U;
    scalar_constant_instruction.source_registers = {5U, 2U, 1U};
    scalar_constant_instruction.source_is_temporary = {false, true, true};
    const auto scalar_constant = execute_shader_alu_add_mul_pair(
        scalar_constant_instruction, registers, constants);
    check(scalar_constant && scalar_constant.previous_scalar == 5.0F &&
              scalar_constant.registers[5] == std::array<float, 4>{5.0F, 5.0F, 5.0F, 5.0F},
          "Xenos scalar ADDS resolves its absolute float constant from src3");

    std::vector<std::uint32_t> register_file(0x4000U + 256U * 4U, 0U);
    for (std::size_t lane = 0U; lane < constants[5].size(); ++lane) {
        register_file[0x4000U + 5U * 4U + lane] =
            std::bit_cast<std::uint32_t>(constants[5][lane]);
    }
    constexpr std::array<std::uint8_t, 1> required_constants{5U};
    const auto decoded_bank = decode_shader_alu_float_constant_bank(
        register_file, required_constants);
    const auto missing_bank = decode_shader_alu_float_constant_bank(
        std::span<const std::uint32_t>(register_file.data(), 0x4000U), required_constants);
    check(decoded_bank && decoded_bank.constants[5] == constants[5] && !missing_bank,
          "PM4 ALU constant register dwords decode to the requested IEEE float4 values");
}

void test_shader_vector_alu_from_encoded_temporaries() {
    using namespace project360::gpu::xenos;
    ShaderAluTemporaryRegisterBank registers{};
    registers[2] = {1.0F, 2.0F, 3.0F, 4.0F};
    registers[3] = {10.0F, 20.0F, 30.0F, 40.0F};
    const std::array<float, 4> previous{};

    // ADD r0, r2, r3.yzwx with both source selectors set to temporaries.
    const auto instruction = decode_shader_alu_instruction(
        {0x000f0000U, 0x00005500U, 0x60000302U});
    const auto result = evaluate_shader_vector_alu_from_temporaries(
        instruction, registers, previous);
    check(result && result.destination == std::array<float, 4>{21.0F, 32.0F, 43.0F, 14.0F},
          "encoded Xenos ADD resolves temporary sources and swizzle before vector evaluation");

    const auto unsupported_source = decode_shader_alu_instruction(
        {0x000f0000U, 0U, 0x20000302U});
    const auto rejected = evaluate_shader_vector_alu_from_temporaries(
        unsupported_source, registers, previous);
    check(!rejected && rejected.destination == std::array<float, 4>{},
          "encoded vector evaluation rejects an unresolved constant source without a result");

    auto conditional_registers = registers;
    conditional_registers[2] = {1.0F, -2.0F, 0.0F, -1.0F};
    conditional_registers[4] = {100.0F, 200.0F, 300.0F, 400.0F};
    const auto encoded_conditional = decode_shader_alu_instruction({0x000f0000U, 0U, 0xed040302U});
    const auto conditional = evaluate_shader_vector_alu_from_temporaries(
        encoded_conditional, conditional_registers, previous);
    check(conditional &&
              conditional.destination == std::array<float, 4>{10.0F, 200.0F, 30.0F, 400.0F},
          "encoded Xenos CNDGE resolves and selects its third temporary source");
}

void test_shader_alu_add_mul_pair_execution() {
    using namespace project360::gpu::xenos;
    ShaderAluTemporaryRegisterBank registers{};
    registers[1] = {1.0F, 2.0F, 3.0F, 4.0F};
    registers[2] = {10.0F, 20.0F, 30.0F, 40.0F};
    registers[3] = {5.0F, 6.0F, 7.0F, 8.0F};

    const auto encode_pair = [](std::uint8_t vector_opcode, std::uint8_t scalar_opcode,
                                std::uint8_t vector_destination,
                                std::uint8_t scalar_destination,
                                std::uint8_t vector_mask, std::uint8_t scalar_mask) {
        const auto word0 = static_cast<std::uint32_t>(vector_destination) |
                           (static_cast<std::uint32_t>(scalar_destination) << 8U) |
                           (static_cast<std::uint32_t>(vector_mask) << 16U) |
                           (static_cast<std::uint32_t>(scalar_mask) << 20U) |
                           (static_cast<std::uint32_t>(scalar_opcode) << 26U);
        const auto word2 = 0xe0030000U |
                           (static_cast<std::uint32_t>(vector_opcode) << 24U) |
                           (2U << 8U) | 1U;
        return decode_shader_alu_instruction({word0, 0U, word2});
    };

    const auto add_adds = encode_pair(0U, 0U, 4U, 5U, 0x0fU, 0x05U);
    const auto added = execute_shader_alu_add_mul_pair(add_adds, registers);
    check(added && added.registers[4] == std::array<float, 4>{11.0F, 22.0F, 33.0F, 44.0F} &&
              added.registers[5] == std::array<float, 4>{13.0F, 0.0F, 13.0F, 0.0F} &&
              added.previous_scalar == 13.0F,
          "Xenos vector ADD and scalar ADDS compute from original sources and commit masked outputs");

    auto initial_outputs = ShaderVertexOutputs{};
    initial_outputs.interpolators[3] = {10.0F, 20.0F, 30.0F, 40.0F};
    initial_outputs.interpolator_write_masks[3] = 0x02U;
    auto position_export = encode_pair(0U, 0U, 62U, 5U, 0x0fU, 0U);
    position_export.export_result = true;
    const auto position = execute_shader_alu_add_mul_pair(
        position_export, registers, {}, 0.0F, initial_outputs);
    auto interpolator_export = encode_pair(0U, 0U, 3U, 5U, 0x05U, 0U);
    interpolator_export.export_result = true;
    const auto interpolator = execute_shader_alu_add_mul_pair(
        interpolator_export, registers, {}, 0.0F, initial_outputs);
    const auto standalone_export_rejected = evaluate_shader_vector_alu(
        position_export, registers[1], registers[2], {}, registers[3]);
    auto unsupported_export = position_export;
    unsupported_export.vector_destination = 63U;
    const auto rejected_export = execute_shader_alu_add_mul_pair(
        unsupported_export, registers, {}, 0.0F, initial_outputs);
    check(position && position.vertex_outputs.position ==
                          std::array<float, 4>{11.0F, 22.0F, 33.0F, 44.0F} &&
              position.vertex_outputs.position_write_mask == 0x0fU &&
              position.registers[62] == registers[62] && interpolator &&
              interpolator.vertex_outputs.interpolators[3] ==
                  std::array<float, 4>{11.0F, 20.0F, 33.0F, 40.0F} &&
              interpolator.vertex_outputs.interpolator_write_masks[3] == 0x07U &&
              !standalone_export_rejected &&
              !rejected_export &&
              rejected_export.error == "Unsupported Xenos vertex shader export register 63",
          "Xenos vertex exports commit position/interpolator lanes and reject unknown targets");

    const auto scalar_state_only = encode_pair(0U, 0U, 4U, 5U, 0x0fU, 0U);
    const auto scalar_state = execute_shader_alu_add_mul_pair(scalar_state_only, registers);
    check(scalar_state && scalar_state.previous_scalar == 13.0F &&
              scalar_state.registers[5] == registers[5],
          "Xenos ADDS updates the previous-scalar state when its destination mask is empty");

    const auto add_previous = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 1U, 4U, 5U, 0U, 0x0fU), registers, {}, 2.0F);
    const auto multiply_previous = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 3U, 4U, 5U, 0U, 0x0fU), registers, {}, 2.0F);
    check(add_previous && add_previous.previous_scalar == 10.0F &&
              add_previous.registers[5] == std::array<float, 4>{10.0F, 10.0F, 10.0F, 10.0F} &&
              multiply_previous && multiply_previous.previous_scalar == 16.0F &&
              multiply_previous.registers[5] == std::array<float, 4>{16.0F, 16.0F, 16.0F, 16.0F},
          "Xenos ADDS_PREV and MULS_PREV consume src3.w and the incoming ps value");

    auto lit_registers = registers;
    lit_registers[3][3] = 3.0F;
    lit_registers[3][0] = 1.0F;
    const auto lit_product = execute_shader_alu_add_mul_pair(encode_pair(0U, 4U, 4U, 5U, 0U, 0x0fU),
                                                             lit_registers, {}, 2.0F);
    const auto lit_sentinel =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 4U, 4U, 5U, 0U, 0x0fU), lit_registers, {},
                                        -std::numeric_limits<float>::max());
    const auto lit_nonfinite_previous =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 4U, 4U, 5U, 0U, 0x0fU), lit_registers, {},
                                        std::numeric_limits<float>::infinity());
    auto lit_invalid_exponent_registers = lit_registers;
    lit_invalid_exponent_registers[3][0] = std::numeric_limits<float>::quiet_NaN();
    const auto lit_invalid_exponent = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 4U, 4U, 5U, 0U, 0x0fU), lit_invalid_exponent_registers, {}, 2.0F);
    auto lit_zero_exponent_registers = lit_registers;
    lit_zero_exponent_registers[3][0] = -0.0F;
    const auto lit_zero_exponent = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 4U, 4U, 5U, 0U, 0x0fU), lit_zero_exponent_registers, {}, 2.0F);
    auto lit_zero_mantissa_registers = lit_registers;
    lit_zero_mantissa_registers[3][3] = -0.0F;
    const auto lit_zero_mantissa = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 4U, 4U, 5U, 0U, 0x0fU), lit_zero_mantissa_registers, {}, 2.0F);
    check(lit_product && lit_product.previous_scalar == 6.0F &&
              lit_product.registers[5] == std::array<float, 4>{6.0F, 6.0F, 6.0F, 6.0F} &&
              lit_sentinel && lit_sentinel.previous_scalar == -std::numeric_limits<float>::max() &&
              lit_nonfinite_previous &&
              lit_nonfinite_previous.previous_scalar == -std::numeric_limits<float>::max() &&
              lit_invalid_exponent &&
              lit_invalid_exponent.previous_scalar == -std::numeric_limits<float>::max() &&
              lit_zero_exponent &&
              lit_zero_exponent.previous_scalar == -std::numeric_limits<float>::max() &&
              lit_zero_mantissa && lit_zero_mantissa.previous_scalar == 0.0F &&
              !std::signbit(lit_zero_mantissa.previous_scalar),
          "Xenos MULS_PREV2 multiplies valid LIT inputs and applies sentinel guards");

    const auto maximum_scalar =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 5U, 4U, 5U, 0U, 0x0fU), registers);
    const auto minimum_scalar =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 6U, 4U, 5U, 0U, 0x0fU), registers);
    const auto equal_scalar =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 7U, 4U, 5U, 0U, 0x0fU), registers);
    const auto greater_scalar =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 8U, 4U, 5U, 0U, 0x0fU), registers);
    const auto greater_equal_scalar =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 9U, 4U, 5U, 0U, 0x0fU), registers);
    const auto not_equal_scalar =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 10U, 4U, 5U, 0U, 0x0fU), registers);
    check(maximum_scalar && maximum_scalar.previous_scalar == 8.0F && minimum_scalar &&
              maximum_scalar.registers[5] == std::array<float, 4>{8.0F, 8.0F, 8.0F, 8.0F} &&
              minimum_scalar.previous_scalar == 5.0F &&
              minimum_scalar.registers[5] == std::array<float, 4>{5.0F, 5.0F, 5.0F, 5.0F} &&
              equal_scalar && equal_scalar.previous_scalar == 0.0F &&
              equal_scalar.registers[5] == std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F} &&
              greater_scalar && greater_scalar.previous_scalar == 1.0F && greater_equal_scalar &&
              greater_equal_scalar.previous_scalar == 1.0F && not_equal_scalar &&
              not_equal_scalar.previous_scalar == 1.0F,
          "Xenos MAXS/MINS and scalar comparisons use src3.w/src3.x as documented");

    auto signed_zero_registers = registers;
    signed_zero_registers[3][3] = -0.0F;
    signed_zero_registers[3][0] = 0.0F;
    const auto signed_zero_maximum = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 5U, 4U, 5U, 0U, 0x0fU), signed_zero_registers);
    const auto signed_zero_minimum = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 6U, 4U, 5U, 0U, 0x0fU), signed_zero_registers);
    const auto signed_zero_equal = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 7U, 4U, 5U, 0U, 0x0fU), signed_zero_registers);
    check(signed_zero_maximum && std::signbit(signed_zero_maximum.previous_scalar) &&
              signed_zero_minimum && !std::signbit(signed_zero_minimum.previous_scalar) &&
              signed_zero_equal && signed_zero_equal.previous_scalar == 1.0F,
          "Xenos scalar compare-select and SEQS preserve the documented signed-zero choices");

    auto nan_registers = registers;
    nan_registers[3][3] = std::numeric_limits<float>::quiet_NaN();
    const auto nan_maximum =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 5U, 4U, 5U, 0U, 0x0fU), nan_registers);
    const auto nan_minimum =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 6U, 4U, 5U, 0U, 0x0fU), nan_registers);
    const auto nan_not_equal =
        execute_shader_alu_add_mul_pair(encode_pair(0U, 10U, 4U, 5U, 0U, 0x0fU), nan_registers);
    check(nan_maximum && nan_maximum.previous_scalar == 5.0F && nan_minimum &&
              nan_minimum.previous_scalar == 5.0F &&
              nan_maximum.registers[5] == std::array<float, 4>{5.0F, 5.0F, 5.0F, 5.0F} &&
              nan_minimum.registers[5] == std::array<float, 4>{5.0F, 5.0F, 5.0F, 5.0F} &&
              nan_not_equal && nan_not_equal.previous_scalar == 1.0F &&
              nan_not_equal.registers[5] == std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F},
          "Xenos scalar compare-select and SNES follow the documented unordered expressions");

    auto negative_fraction_registers = registers;
    negative_fraction_registers[3][3] = -1.25F;
    const auto scalar_fraction = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 11U, 4U, 5U, 0U, 0x0fU), negative_fraction_registers);
    const auto scalar_truncate = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 12U, 4U, 5U, 0U, 0x0fU), negative_fraction_registers);
    const auto scalar_floor = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 13U, 4U, 5U, 0U, 0x0fU), negative_fraction_registers);
    check(scalar_fraction && scalar_fraction.previous_scalar == 0.75F &&
              scalar_fraction.registers[5] == std::array<float, 4>{0.75F, 0.75F, 0.75F, 0.75F} &&
              scalar_truncate && scalar_truncate.previous_scalar == -1.0F && scalar_floor &&
              scalar_floor.previous_scalar == -2.0F,
          "Xenos FRCS/TRUNCS/FLOORS apply floor and truncation to src3.w");

    auto reciprocal_registers = registers;
    reciprocal_registers[3][3] = 2.0F;
    const auto rcpc_finite = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 17U, 4U, 5U, 0U, 0x0fU), reciprocal_registers);
    const auto rcpf_finite = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 18U, 4U, 5U, 0U, 0x0fU), reciprocal_registers);
    auto reciprocal_positive_zero_registers = reciprocal_registers;
    reciprocal_positive_zero_registers[3][3] = 0.0F;
    const auto rcpc_positive_zero = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 17U, 4U, 5U, 0U, 0x0fU), reciprocal_positive_zero_registers);
    const auto rcpf_positive_zero = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 18U, 4U, 5U, 0U, 0x0fU), reciprocal_positive_zero_registers);
    auto reciprocal_negative_zero_registers = reciprocal_registers;
    reciprocal_negative_zero_registers[3][3] = -0.0F;
    const auto rcpc_negative_zero = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 17U, 4U, 5U, 0U, 0x0fU), reciprocal_negative_zero_registers);
    const auto rcpf_negative_zero = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 18U, 4U, 5U, 0U, 0x0fU), reciprocal_negative_zero_registers);
    auto reciprocal_nan_registers = reciprocal_registers;
    reciprocal_nan_registers[3][3] = std::numeric_limits<float>::quiet_NaN();
    const auto rcpc_nan = execute_shader_alu_add_mul_pair(encode_pair(0U, 17U, 4U, 5U, 0U, 0x0fU),
                                                          reciprocal_nan_registers);
    const auto rcpf_nan = execute_shader_alu_add_mul_pair(encode_pair(0U, 18U, 4U, 5U, 0U, 0x0fU),
                                                          reciprocal_nan_registers);
    auto reciprocal_positive_infinity_registers = reciprocal_registers;
    reciprocal_positive_infinity_registers[3][3] = std::numeric_limits<float>::infinity();
    const auto rcpc_positive_infinity = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 17U, 4U, 5U, 0U, 0x0fU), reciprocal_positive_infinity_registers);
    auto reciprocal_negative_infinity_registers = reciprocal_registers;
    reciprocal_negative_infinity_registers[3][3] = -std::numeric_limits<float>::infinity();
    const auto rcpf_negative_infinity = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 18U, 4U, 5U, 0U, 0x0fU), reciprocal_negative_infinity_registers);
    auto reciprocal_one_registers = reciprocal_registers;
    reciprocal_one_registers[3][3] = 1.0F;
    const auto rcpc_one = execute_shader_alu_add_mul_pair(encode_pair(0U, 17U, 4U, 5U, 0U, 0x0fU),
                                                          reciprocal_one_registers);
    const auto rcp_ieee = execute_shader_alu_add_mul_pair(encode_pair(0U, 19U, 4U, 5U, 0U, 0x0fU),
                                                          reciprocal_registers);
    check(rcpc_finite && rcpc_finite.previous_scalar == 0.5F && rcpf_finite &&
              rcpf_finite.previous_scalar == 0.5F && rcpc_positive_zero &&
              rcpc_positive_zero.previous_scalar == std::numeric_limits<float>::max() &&
              rcpf_positive_zero && rcpf_positive_zero.previous_scalar == 0.0F &&
              !std::signbit(rcpf_positive_zero.previous_scalar) && rcpc_negative_zero &&
              rcpc_negative_zero.previous_scalar == -std::numeric_limits<float>::max() &&
              rcpf_negative_zero && rcpf_negative_zero.previous_scalar == 0.0F &&
              std::signbit(rcpf_negative_zero.previous_scalar) && rcpc_nan &&
              std::isnan(rcpc_nan.previous_scalar) && rcpf_nan &&
              std::isnan(rcpf_nan.previous_scalar) && rcpc_positive_infinity &&
              rcpc_positive_infinity.previous_scalar == 0.0F && rcpf_negative_infinity &&
              rcpf_negative_infinity.previous_scalar == 0.0F &&
              std::signbit(rcpf_negative_infinity.previous_scalar) && rcpc_one &&
              rcpc_one.previous_scalar == 1.0F && !rcp_ieee,
          "Xenos RCPC/RCPF clamp reciprocal overflow, preserve signed zero and NaN, and "
          "special-case one");

    const auto multiply_pair = encode_pair(1U, 2U, 1U, 1U, 0x03U, 0x0cU);
    const auto multiplied = execute_shader_alu_add_mul_pair(multiply_pair, registers);
    check(multiplied &&
              multiplied.registers[1] == std::array<float, 4>{10.0F, 40.0F, 40.0F, 40.0F} &&
              multiplied.previous_scalar == 40.0F,
          "Xenos vector MUL and scalar MULS merge disjoint masks on the same destination");

    const auto maximum_pair = encode_pair(2U, 0U, 4U, 5U, 0x0fU, 0x0fU);
    const auto maximized = execute_shader_alu_add_mul_pair(maximum_pair, registers);
    check(maximized && maximized.registers[4] == std::array<float, 4>{10.0F, 20.0F, 30.0F, 40.0F} &&
              maximized.previous_scalar == 13.0F,
          "Xenos ALU pair executes vector MAX alongside scalar ADDS");

    auto conditional_registers = registers;
    conditional_registers[1] = {1.0F, -2.0F, 0.0F, -1.0F};
    const auto conditional_pair = execute_shader_alu_add_mul_pair(
        encode_pair(13U, 0U, 4U, 5U, 0x0fU, 0U), conditional_registers);
    check(conditional_pair &&
              conditional_pair.registers[4] == std::array<float, 4>{10.0F, 6.0F, 30.0F, 8.0F},
          "Xenos ALU pair executes CNDGE using vector src2/src3 lanes");

    const auto compare_pair = encode_pair(7U, 0U, 4U, 5U, 0x0fU, 0x0fU);
    const auto compared = execute_shader_alu_add_mul_pair(compare_pair, registers);
    check(compared && compared.registers[4] == std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F},
          "Xenos ALU pair executes vector SNE comparisons alongside scalar ADDS");

    const auto dot_pair = encode_pair(16U, 0U, 4U, 5U, 0x0fU, 0x0fU);
    const auto dotted = execute_shader_alu_add_mul_pair(dot_pair, registers);
    check(dotted && dotted.registers[4] == std::array<float, 4>{140.0F, 140.0F, 140.0F, 140.0F} &&
              dotted.previous_scalar == 13.0F,
          "Xenos ALU pair executes DP3 alongside scalar ADDS");

    const auto dot2add_pair = encode_pair(17U, 0U, 4U, 5U, 0x0fU, 0x0fU);
    const auto dot2added = execute_shader_alu_add_mul_pair(dot2add_pair, registers);
    check(dot2added && dot2added.registers[4] == std::array<float, 4>{55.0F, 55.0F, 55.0F, 55.0F} &&
              dot2added.previous_scalar == 13.0F,
          "Xenos ALU pair executes DP2ADD with its src3.x addend alongside scalar ADDS");

    auto unordered_registers = registers;
    unordered_registers[2][1] = std::numeric_limits<float>::quiet_NaN();
    const auto unordered_pair = encode_pair(3U, 0U, 4U, 5U, 0x0fU, 0x0fU);
    const auto unordered_result =
        execute_shader_alu_add_mul_pair(unordered_pair, unordered_registers);
    check(!unordered_result && unordered_result.registers == ShaderAluTemporaryRegisterBank{},
          "Xenos ALU pair rejects unverified MAX/MIN NaN behavior without partial writes");

    auto special_registers = registers;
    special_registers[3] = {std::numeric_limits<float>::infinity(), 6.0F, 7.0F,
                            std::numeric_limits<float>::denorm_min()};
    const auto special_multiply = execute_shader_alu_add_mul_pair(
        encode_pair(0U, 2U, 4U, 5U, 0U, 0x0fU), special_registers);
    check(special_multiply && special_multiply.previous_scalar == 0.0F &&
              !std::signbit(special_multiply.previous_scalar),
          "Xenos scalar MULS flushes a subnormal operand to positive zero");

    const auto unsupported_pair = encode_pair(0U, 14U, 4U, 5U, 0x0fU, 0x0fU);
    const auto unsupported = execute_shader_alu_add_mul_pair(unsupported_pair, registers);
    const auto conflicting_pair = encode_pair(0U, 0U, 4U, 4U, 0x03U, 0x01U);
    const auto conflicting = execute_shader_alu_add_mul_pair(conflicting_pair, registers);
    check(!unsupported && unsupported.registers == ShaderAluTemporaryRegisterBank{} &&
              !conflicting && conflicting.registers == ShaderAluTemporaryRegisterBank{},
          "Xenos ALU pair rejects unsupported scalar state and overlapping paired writes atomically");
}

void test_shader_exec_block_ordered_alu_and_vfetch() {
    using namespace project360::gpu::xenos;
    const std::uint32_t first_word0 = 0x123U | (3U << 12U) | (0xabcU << 16U);
    const std::uint32_t first_word1 = 1U << 12U;
    const std::uint32_t second_word0 = 0x456U | (1U << 12U) | (0x789U << 16U);
    const std::uint32_t second_word1 = 2U << 12U;
    const std::array<std::array<std::uint32_t, 3>, 1> packed_control_flow{{
        {first_word0, first_word1 | (second_word0 << 16U),
         (second_word0 >> 16U) | (second_word1 << 16U)}}};
    const auto decoded_pair = decode_shader_exec_pairs(packed_control_flow);
    check(decoded_pair && decoded_pair.instructions.size() == 2U &&
              decoded_pair.instructions[0].instruction_address == 0x123U &&
              decoded_pair.instructions[0].instruction_count == 3U &&
              decoded_pair.instructions[0].sequence == 0xabcU &&
              decoded_pair.instructions[0].opcode == 1U &&
              decoded_pair.instructions[1].instruction_address == 0x456U &&
              decoded_pair.instructions[1].instruction_count == 1U &&
              decoded_pair.instructions[1].sequence == 0x789U &&
              decoded_pair.instructions[1].opcode == 2U,
          "Xenos packed three-dword control flow unpacks both EXEC headers by literal fields");
    const auto empty_control_flow = decode_shader_exec_pairs({});
    std::vector<std::array<std::uint32_t, 3>> oversized_control_flow(2049U);
    const auto oversized_result = decode_shader_exec_pairs(oversized_control_flow);
    check(!empty_control_flow && !oversized_result &&
              empty_control_flow.instructions.empty() && oversized_result.instructions.empty(),
          "Xenos packed control-flow decoder rejects empty and oversized inputs without partial output");

    const auto pack_control_flow = [](std::uint32_t first_word0,
                                      std::uint32_t first_word1,
                                      std::uint32_t second_word0,
                                      std::uint32_t second_word1) {
        return std::array<std::uint32_t, 3>{
            first_word0, first_word1 | (second_word0 << 16U),
            (second_word0 >> 16U) | (second_word1 << 16U)};
    };
    const auto exec_header = pack_control_flow(1U | (1U << 12U), 1U << 12U,
                                               1U | (1U << 12U), 2U << 12U);
    const std::array<std::array<std::uint32_t, 3>, 2> complete_ucode{
        exec_header, std::array<std::uint32_t, 3>{0U, 0U, 0U}};
    const auto shader_program = decode_shader_exec_program(complete_ucode, 1U);
    check(shader_program && shader_program.control_flow_record_count == 1U &&
              shader_program.control_flow.size() == 2U &&
              shader_program.control_flow[0].instruction_address == 1U &&
              shader_program.control_flow[1].opcode == 2U,
          "Xenos complete shader parser validates its explicit control-flow prefix and absolute EXEC address");
    const auto oversized_prefix = decode_shader_exec_program(complete_ucode, 3U);
    auto invalid_address_ucode = complete_ucode;
    invalid_address_ucode[0] = pack_control_flow(2U | (1U << 12U), 1U << 12U,
                                                  1U | (1U << 12U), 2U << 12U);
    const auto invalid_address_program = decode_shader_exec_program(invalid_address_ucode, 1U);
    check(!oversized_prefix && !invalid_address_program &&
              oversized_prefix.control_flow.empty() && invalid_address_program.control_flow.empty(),
          "Xenos complete shader parser rejects out-of-buffer prefix and EXEC ranges transactionally");

    project360::memory::GuestMemory memory(128U);
    const std::array<std::byte, 32> vertex_data{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x40},
        std::byte{0}, std::byte{0}, std::byte{0x40}, std::byte{0x40},
        std::byte{0}, std::byte{0}, std::byte{0x80}, std::byte{0x40},
        std::byte{0}, std::byte{0}, std::byte{0xa0}, std::byte{0x40}};
    check(memory.load(64U, vertex_data), "shader EXEC fixture loads vertex data");

    const auto descriptor = decode_vertex_fetch_descriptor((16U << 2U) | 3U, 8U << 2U);
    std::array<VertexFetchDescriptor, 8> fetch_constants{};
    if (descriptor) {
        fetch_constants[7] = descriptor.descriptor;
    }

    const std::array<std::array<std::uint32_t, 3>, 3> ucode{
        std::array<std::uint32_t, 3>{
            3U | (1U << 7U) | (0x0fU << 16U), 0U,
            (1U << 31U) | (3U << 16U) | (1U << 30U) | (2U << 8U) |
                5U},
        std::array<std::uint32_t, 3>{
            (3U << 5U) | (4U << 12U) | (1U << 19U) | (2U << 20U) | (1U << 25U) |
                (2U << 27U) | (2U << 30U),
            0x00000688U | (38U << 16U) | (1U << 15U) | (1U << 14U) |
                (1U << 13U) | (1U << 12U),
            4U},
        std::array<std::uint32_t, 3>{
            10U | (0x0fU << 16U), 0U,
            (1U << 31U) | (3U << 16U) | (1U << 30U) | (4U << 8U) | 4U |
                (1U << 29U) | (1U << 24U)}};

    auto executable_complete_ucode = complete_ucode;
    executable_complete_ucode[1U] = ucode[0U];
    const auto runnable_shader = decode_shader_exec_program(executable_complete_ucode, 1U);
    ShaderAluFloatConstantBank runnable_constants{};
    runnable_constants[5U] = {0.0F, 0.0F, -1.0F, 0.0F};
    const auto runnable_result = runnable_shader
                                     ? execute_shader_exec_sequence(
                                           runnable_shader.control_flow, executable_complete_ucode,
                                           fetch_constants, memory, {}, {}, runnable_constants)
                                     : ShaderExecSequenceResult{};
    if (!runnable_shader || !runnable_result) {
        std::cerr << "Xenos complete shader execution diagnostic: "
                  << (runnable_shader ? runnable_result.error : runnable_shader.error) << '\n';
    }
    check(runnable_shader && runnable_result && runnable_result.ended_shader &&
              runnable_result.instructions_executed == 2U,
          "Xenos parsed EXEC addresses run directly against the complete unrebased microcode array");

    // Two sequence bits per instruction: ALU, VFETCH, ALU. The serialize bit
    // is retained in metadata; this reference executor commits each operation
    // synchronously in sequence order.
    constexpr std::uint32_t sequence = 0b10U | (0b11U << 2U) | (0b10U << 4U);
    const auto exec = decode_shader_exec_instruction(
        {sequence << 16U | (3U << 12U), 2U << 12U});
    ShaderAluTemporaryRegisterBank initial_registers{};
    ShaderAluFloatConstantBank float_constants{};
    float_constants[5] = {0.0F, 0.0F, -1.0F, 0.0F};
    const auto result = descriptor
                            ? execute_shader_exec_block(exec, ucode, fetch_constants, memory,
                                                        initial_registers, {}, float_constants)
                            : ShaderExecResult{{}, 0U, false, "Invalid descriptor fixture"};
    check(result && result.instructions_executed == 3U && result.ended_shader &&
              result.registers[3] == std::array<float, 4>{0.0F, 0.0F, 1.0F, 0.0F} &&
              result.registers[4] == std::array<float, 4>{2.0F, 3.0F, 4.0F, 5.0F} &&
              result.registers[10] == std::array<float, 4>{4.0F, 9.0F, 16.0F, 25.0F},
          "Xenos EXEC sequence runs ALU, VFETCH and dependent ALU operations in order");

    const std::array<std::array<std::uint32_t, 3>, 1> export_ucode{
        std::array<std::uint32_t, 3>{62U | (1U << 15U) | (0x0fU << 16U), 0U,
                                    0xe0030000U | (2U << 8U) | 1U}};
    const auto export_exec = decode_shader_exec_instruction({1U << 12U, 2U << 12U});
    auto export_registers = initial_registers;
    export_registers[1] = {1.0F, 2.0F, 3.0F, 4.0F};
    export_registers[2] = {10.0F, 20.0F, 30.0F, 40.0F};
    const auto exported = execute_shader_exec_block(
        export_exec, export_ucode, fetch_constants, memory, export_registers, {});
    check(exported && exported.ended_shader && exported.instructions_executed == 1U &&
              exported.vertex_outputs.position ==
                  std::array<float, 4>{11.0F, 22.0F, 33.0F, 44.0F} &&
              exported.vertex_outputs.position_write_mask == 0x0fU,
          "Xenos EXEC propagates ALU position exports to the vertex-output result");

    auto scalar_registers = initial_registers;
    scalar_registers[3] = {5.0F, 0.0F, 0.0F, 8.0F};
    const auto scalar_word2 = (7U << 29U) | (3U << 16U) | (2U << 8U) | 1U;
    const std::array<std::array<std::uint32_t, 3>, 2> scalar_ucode{
        std::array<std::uint32_t, 3>{(5U << 8U) | (0x0fU << 20U), 0U, scalar_word2},
        std::array<std::uint32_t, 3>{(6U << 8U) | (0x0fU << 20U) | (1U << 26U), 0U, scalar_word2}};
    constexpr std::uint32_t scalar_sequence = 0b10U | (0b10U << 2U);
    const auto scalar_exec =
        decode_shader_exec_instruction({scalar_sequence << 16U | (2U << 12U), 2U << 12U});
    const auto scalar_result = execute_shader_exec_block(scalar_exec, scalar_ucode, fetch_constants,
                                                         memory, scalar_registers, {});
    check(scalar_result && scalar_result.previous_scalar == 21.0F &&
              scalar_result.registers[5] == std::array<float, 4>{13.0F, 13.0F, 13.0F, 13.0F} &&
              scalar_result.registers[6] == std::array<float, 4>{21.0F, 21.0F, 21.0F, 21.0F},
          "Xenos EXEC carries scalar previous-result state between ordered ALU slots");

    const std::array<ShaderExecInstruction, 2> scalar_blocks{
        decode_shader_exec_instruction({(0b10U << 16U) | (1U << 12U), 1U << 12U}),
        decode_shader_exec_instruction({1U | (0b10U << 16U) | (1U << 12U), 2U << 12U}),
    };
    const auto scalar_program = execute_shader_exec_sequence(
        scalar_blocks, scalar_ucode, fetch_constants, memory, scalar_registers, {});
    check(scalar_program && scalar_program.ended_shader &&
              scalar_program.blocks_executed == 2U && scalar_program.instructions_executed == 2U &&
              scalar_program.previous_scalar == scalar_result.previous_scalar &&
              scalar_program.registers[5] == scalar_result.registers[5] &&
              scalar_program.registers[6] == scalar_result.registers[6],
          "Xenos EXEC sequence carries register and scalar state across blocks to EXEC_END");

    const std::array<ShaderExecInstruction, 1> unterminated_blocks{
        decode_shader_exec_instruction({(0b10U << 16U) | (1U << 12U), 1U << 12U}),
    };
    const auto unterminated_program = execute_shader_exec_sequence(
        unterminated_blocks, scalar_ucode, fetch_constants, memory, scalar_registers, {});
    check(!unterminated_program && !unterminated_program.ended_shader &&
              unterminated_program.registers == ShaderAluTemporaryRegisterBank{} &&
              unterminated_program.error.find("without EXEC_END") != std::string::npos,
          "Xenos EXEC sequence rejects missing shader termination without partial state");

    std::array<bool, 4> boolean_constants{false, false, false, true};
    const auto conditional_words = std::array<std::uint32_t, 2>{
        scalar_sequence << 16U | (2U << 12U), (3U << 12U) | (1U << 11U) | (1U << 8U) | 3U};
    const auto conditional_exec = decode_shader_exec_instruction(conditional_words);
    const auto conditional_result =
        execute_shader_exec_block(conditional_exec, scalar_ucode, fetch_constants, memory,
                                  scalar_registers, {}, {}, 0.0F, boolean_constants);
    check(conditional_exec.boolean_constant_address == 3U && conditional_exec.condition &&
              conditional_result && conditional_result.instructions_executed == 2U &&
              conditional_result.registers[5] == scalar_result.registers[5],
          "Xenos COND_EXEC evaluates the selected bool constant and runs a matching block");

    auto skipped_conditional_words = conditional_words;
    skipped_conditional_words[1] &= ~(1U << 8U);
    const auto skipped_conditional = decode_shader_exec_instruction(skipped_conditional_words);
    const auto skipped_result =
        execute_shader_exec_block(skipped_conditional, scalar_ucode, fetch_constants, memory,
                                  scalar_registers, {}, {}, 0.0F, boolean_constants);
    check(skipped_result && skipped_result.instructions_executed == 0U &&
              !skipped_result.ended_shader && skipped_result.registers == scalar_registers,
          "Xenos COND_EXEC skips a mismatched block without changing registers");

    auto false_condition_words = conditional_words;
    false_condition_words[1] &= ~0x1ffU;
    const auto false_condition_exec = decode_shader_exec_instruction(false_condition_words);
    const auto false_condition_result = execute_shader_exec_block(
        false_condition_exec, scalar_ucode, fetch_constants, memory, scalar_registers, {}, {}, 0.0F,
        boolean_constants);
    check(false_condition_result && !false_condition_exec.condition &&
              false_condition_result.instructions_executed == 2U,
          "Xenos COND_EXEC executes when a false bool constant matches the condition");

    auto conditional_end_words = conditional_words;
    conditional_end_words[1] = (conditional_end_words[1] & ~(0xfU << 12U)) | (4U << 12U);
    const auto conditional_end = decode_shader_exec_instruction(conditional_end_words);
    const auto conditional_end_result =
        execute_shader_exec_block(conditional_end, scalar_ucode, fetch_constants, memory,
                                  scalar_registers, {}, {}, 0.0F, boolean_constants);
    conditional_end_words[1] &= ~(1U << 8U);
    const auto skipped_conditional_end = decode_shader_exec_instruction(conditional_end_words);
    const auto skipped_end_result =
        execute_shader_exec_block(skipped_conditional_end, scalar_ucode, fetch_constants, memory,
                                  scalar_registers, {}, {}, 0.0F, boolean_constants);
    check(conditional_end_result && conditional_end_result.ended_shader && skipped_end_result &&
              !skipped_end_result.ended_shader && skipped_end_result.instructions_executed == 0U,
          "Xenos COND_EXEC_END terminates only when its bool condition matches");

    const auto missing_boolean = execute_shader_exec_block(
        conditional_exec, scalar_ucode, fetch_constants, memory, scalar_registers, {}, {}, 0.0F);
    auto relative_conditional_words = conditional_words;
    relative_conditional_words[1] &= ~(1U << 11U);
    const auto relative_conditional = decode_shader_exec_instruction(relative_conditional_words);
    const auto rejected_relative_conditional =
        execute_shader_exec_block(relative_conditional, scalar_ucode, fetch_constants, memory,
                                  scalar_registers, {}, {}, 0.0F, boolean_constants);
    check(!missing_boolean && !rejected_relative_conditional,
          "Xenos conditional EXEC rejects missing bool constants and relative addresses");

    auto invalid_ucode = ucode;
    invalid_ucode[2][2] = (invalid_ucode[2][2] & ~(0x1fU << 24U)) | (11U << 24U);
    const auto rejected = execute_shader_exec_block(
        exec, invalid_ucode, fetch_constants, memory, initial_registers, {}, float_constants);
    const auto out_of_range = execute_shader_exec_block(
        decode_shader_exec_instruction({(2U << 12U), 2U << 12U}), ucode,
        fetch_constants, memory, initial_registers, {}, float_constants);
    const auto unsupported_control_flow = execute_shader_exec_block(
        decode_shader_exec_instruction({sequence << 16U | (3U << 12U), 0U}), ucode,
        fetch_constants, memory, initial_registers, {}, float_constants);
    check(!rejected && rejected.instructions_executed == 0U &&
              rejected.registers == ShaderAluTemporaryRegisterBank{} && !out_of_range &&
              !unsupported_control_flow,
          "Xenos EXEC rejects unsupported operations, control flow and ranges without partial state");
}

void test_noops_and_malformed_streams() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> noops;
    append_u32_be(noops, 0U);
    append_u32_be(noops, 0x80000000U);
    const auto decoded_noops = decode_pm4(noops);
    check(static_cast<bool>(decoded_noops) && decoded_noops.packets.size() == 2U &&
              decoded_noops.packets[0].type == PacketType::empty_nop &&
              decoded_noops.packets[1].type == PacketType::type2_nop,
          "zero header and type-2 packet are decoded as no-ops");

    std::vector<std::uint8_t> truncated_type0;
    append_u32_be(truncated_type0, (2U << 16U) | 0x20U); // Three payload words.
    append_u32_be(truncated_type0, 0xabcdef01U);
    const auto type0_error = decode_pm4(truncated_type0);
    check(!type0_error && type0_error.packets.empty() && type0_error.error_word_offset == 0U,
          "truncated type-0 packet fails without exposing partial decode results");

    std::vector<std::uint8_t> truncated_type1;
    append_u32_be(truncated_type1, 0x40000000U);
    append_u32_be(truncated_type1, 0U);
    const auto type1_error = decode_pm4(truncated_type1);
    check(!type1_error && type1_error.packets.empty(),
          "type-1 packet requires both data dwords");

    std::vector<std::uint8_t> truncated_type3;
    append_u32_be(truncated_type3, 0xc0000000U | (0x3fffU << 16U) | (7U << 8U));
    const auto type3_error = decode_pm4(truncated_type3);
    check(!type3_error && type3_error.packets.empty(),
          "maximum declared type-3 payload is rejected when truncated");

    std::vector<std::uint8_t> partial_dword{0U, 1U, 2U};
    const auto alignment_error = decode_pm4(partial_dword);
    check(!alignment_error && alignment_error.packets.empty(),
          "stream with a partial dword is rejected transactionally");
}

void test_register_packet_application() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> stream;
    append_u32_be(stream, (1U << 16U) | 0x0123U);
    append_u32_be(stream, 0x11111111U);
    append_u32_be(stream, 0x22222222U);
    append_u32_be(stream, 0x40000000U | (0x345U << 11U) | 0x123U);
    append_u32_be(stream, 0x33333333U);
    append_u32_be(stream, 0x44444444U);
    append_u32_be(stream, 0x80000000U);
    append_u32_be(stream, 0xc0000000U | (0x10U << 8U));
    append_u32_be(stream, 0x55555555U);

    const auto decoded = decode_pm4(stream);
    std::vector<std::uint32_t> registers(0x400U, 0U);
    if (decoded) {
        const auto applied = apply_register_packets(decoded.packets, stream, registers);
        check(static_cast<bool>(applied), "valid type-0/type-1 register writes apply");
        check(registers[0x123U] == 0x33333333U && registers[0x124U] == 0x22222222U &&
                  registers[0x345U] == 0x44444444U,
              "sequential type-0 and indexed type-1 writes update addressed registers");
        check(registers[0U] == 0U,
              "type-2 no-op and type-3 command payload do not alter register state");
    } else {
        check(false, "register test stream parses before applying writes");
    }

    std::vector<std::uint8_t> repeated_stream;
    append_u32_be(repeated_stream, (1U << 16U) | (1U << 15U) | 0x10U);
    append_u32_be(repeated_stream, 0xaaaa0001U);
    append_u32_be(repeated_stream, 0xaaaa0002U);
    const auto repeated_decoded = decode_pm4(repeated_stream);
    std::vector<std::uint32_t> repeated_registers(0x20U, 0x55555555U);
    const auto repeated_result = apply_register_packets(repeated_decoded.packets,
                                                        repeated_stream, repeated_registers);
    check(static_cast<bool>(repeated_result) && repeated_registers[0x10U] == 0xaaaa0002U &&
              repeated_registers[0x11U] == 0x55555555U,
          "type-0 write-one mode repeatedly targets the same register");

    std::vector<std::uint8_t> xenos_register_stream;
    append_u32_be(xenos_register_stream, (2U << 16U) | 0x2100U);
    append_u32_be(xenos_register_stream, 0xff123456U);
    append_u32_be(xenos_register_stream, 0xffabcdefU);
    append_u32_be(xenos_register_stream, 0xff654321U);
    append_u32_be(xenos_register_stream, 0x21fbU);
    append_u32_be(xenos_register_stream, 0xffffffffU);
    append_u32_be(xenos_register_stream, 0x21fcU);
    append_u32_be(xenos_register_stream, 0xffffffffU);
    append_u32_be(xenos_register_stream, 0x0100U);
    append_u32_be(xenos_register_stream, 0xffffffffU);
    const auto xenos_decoded = decode_pm4(xenos_register_stream);
    std::vector<std::uint32_t> xenos_registers(0x5000U, 0U);
    const auto xenos_applied = apply_register_packets(
        xenos_decoded.packets, xenos_register_stream, xenos_registers);
    check(xenos_applied && xenos_registers[0x2100U] == 0x00123456U &&
              xenos_registers[0x2101U] == 0x00abcdefU &&
              xenos_registers[0x2102U] == 0x00654321U &&
              xenos_registers[0x21fbU] == 0xc0ffffffU &&
              xenos_registers[0x21fcU] == 0xffff1bffU &&
              xenos_registers[0x0100U] == 0xffffffffU,
          "Xenos register map masks reserved draw bits and preserves opaque registers");

    std::vector<std::uint8_t> invalid_stream;
    append_u32_be(invalid_stream, 2U);
    append_u32_be(invalid_stream, 0xdeadbeefU);
    append_u32_be(invalid_stream, 0x40000000U | (4U << 11U) | 1U);
    append_u32_be(invalid_stream, 0x11111111U);
    append_u32_be(invalid_stream, 0x22222222U);
    const auto invalid_decoded = decode_pm4(invalid_stream);
    std::vector<std::uint32_t> protected_registers(4U, 0xabcdef01U);
    const auto invalid_result = apply_register_packets(invalid_decoded.packets,
                                                       invalid_stream, protected_registers);
    check(!invalid_result && protected_registers[0] == 0xabcdef01U &&
              protected_registers[1] == 0xabcdef01U &&
              protected_registers[2] == 0xabcdef01U &&
              protected_registers[3] == 0xabcdef01U,
          "out-of-range later write rejects the complete stream without partial register changes");

    auto stale_stream = stream;
    stale_stream[0] ^= 0x01U;
    std::vector<std::uint32_t> stale_registers(0x400U, 0xabcdef01U);
    const auto stale_result = apply_register_packets(decoded.packets, stale_stream,
                                                     stale_registers);
    check(!stale_result && stale_registers[0x123U] == 0xabcdef01U,
          "register application rejects decoded packet metadata from a different stream");
}

void test_type3_constant_uploads() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> stream;
    const auto add_set_constant = [&](std::uint32_t type, std::uint32_t index,
                                      std::uint32_t value) {
        append_u32_be(stream, 0xc0000000U | (1U << 16U) | (0x2dU << 8U));
        append_u32_be(stream, (type << 16U) | index);
        append_u32_be(stream, value);
    };
    add_set_constant(0U, 3U, 0xa0000000U);
    add_set_constant(1U, 5U, 0xa1000000U);
    add_set_constant(2U, 6U, 0xa2000000U);
    add_set_constant(3U, 1U, 0xa3000000U);
    add_set_constant(4U, 7U, 0xa4000000U);
    append_u32_be(stream, 0xc0000000U | (2U << 16U) | (0x55U << 8U));
    append_u32_be(stream, 0x3456U);
    append_u32_be(stream, 0xb0000000U);
    append_u32_be(stream, 0xb0000001U);
    append_u32_be(stream, 0xc0000000U | (1U << 16U) | (0x56U << 8U));
    append_u32_be(stream, 0x4321U);
    append_u32_be(stream, 0xc0000000U);

    const auto decoded = decode_pm4(stream);
    std::vector<std::uint32_t> registers(0x5000U, 0U);
    if (!decoded) {
        check(false, "constant upload test stream parses");
        return;
    }
    const auto applied = apply_register_packets(decoded.packets, stream, registers);
    check(static_cast<bool>(applied), "supported type-3 constant uploads apply");
    check(registers[0x4003U] == 0xa0000000U && registers[0x4805U] == 0xa1000000U &&
              registers[0x4906U] == 0xa2000000U && registers[0x4909U] == 0xa3000000U &&
              registers[0x2007U] == 0xa4000000U,
          "SET_CONSTANT maps ALU, fetch, boolean, loop and direct register banks");
    check(registers[0x3456U] == 0xb0000000U && registers[0x3457U] == 0xb0000001U &&
              registers[0x4321U] == 0xc0000000U,
          "SET_CONSTANT2 and SET_SHADER_CONSTANTS write direct register indexes");

    std::vector<std::uint8_t> invalid_stream;
    append_u32_be(invalid_stream, 0x00000004U);
    append_u32_be(invalid_stream, 0x11111111U);
    append_u32_be(invalid_stream, 0xc0000000U | (1U << 16U) | (0x2dU << 8U));
    append_u32_be(invalid_stream, (6U << 16U) | 2U); // Unsupported constant bank.
    append_u32_be(invalid_stream, 0x22222222U);
    const auto invalid_decoded = decode_pm4(invalid_stream);
    std::vector<std::uint32_t> unchanged(0x5000U, 0xccccccccU);
    const auto invalid_result = apply_register_packets(invalid_decoded.packets,
                                                       invalid_stream, unchanged);
    check(!invalid_result && unchanged[4U] == 0xccccccccU,
          "unsupported SET_CONSTANT bank rejects prior writes transactionally");
}

void test_unimplemented_commands_fail_explicitly() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> draw_stream;
    append_u32_be(draw_stream, 4U);
    append_u32_be(draw_stream, 0x12345678U);
    append_u32_be(draw_stream, 0xc0000000U | (1U << 16U) | (0x22U << 8U)); // DRAW_INDX.
    append_u32_be(draw_stream, 0U); // Visibility query condition.
    append_u32_be(draw_stream, (3U << 16U) | (2U << 6U)); // Auto-index, triangle list.
    const auto draw_decoded = decode_pm4(draw_stream);
    std::vector<std::uint32_t> draw_registers(8U, 0xccccccccU);
    const auto draw_result = apply_register_packets(draw_decoded.packets, draw_stream,
                                                   draw_registers);
    check(!draw_result && draw_registers[4U] == 0xccccccccU,
          "unimplemented draw command is reported without committing preceding writes");

    std::vector<std::uint8_t> predicated_stream;
    append_u32_be(predicated_stream, 0xc0000000U | (1U << 16U) |
                                       (0x2dU << 8U) | 1U);
    append_u32_be(predicated_stream, 0U);
    append_u32_be(predicated_stream, 0xabcdef01U);
    const auto predicated_decoded = decode_pm4(predicated_stream);
    std::vector<std::uint32_t> predicated_registers(0x5000U, 0xccccccccU);
    const auto predicated_result = apply_register_packets(
        predicated_decoded.packets, predicated_stream, predicated_registers);
    check(!predicated_result && predicated_registers[0x4000U] == 0xccccccccU,
          "predicated constant upload fails until bin-selection state is modeled");

    std::vector<std::uint8_t> complete_stream;
    append_u32_be(complete_stream, 0x80000000U);
    const auto complete_decoded = decode_pm4(complete_stream);
    std::vector<std::uint32_t> complete_registers(1U, 0U);
    const auto incomplete_list_result = apply_register_packets(
        std::span<const Packet>(complete_decoded.packets.data(), 0U), complete_stream,
        complete_registers);
    check(!incomplete_list_result,
          "register application rejects a packet list that omits stream contents");
}

void test_draw_packet_metadata_decoding() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> stream;
    // DRAW_INDX: query token, 16-bit DMA indices, triangle list, 3 indices.
    append_u32_be(stream, 0xc0000000U | (3U << 16U) | (0x22U << 8U));
    append_u32_be(stream, 0x00000105U);
    append_u32_be(stream, (3U << 16U) | 3U);
    append_u32_be(stream, 0x00123456U);
    append_u32_be(stream, 0x00000003U);
    // DRAW_INDX_2: auto-index point list, two indices, with no query token.
    append_u32_be(stream, 0xc0000000U | (0x36U << 8U));
    append_u32_be(stream, (2U << 16U) | (2U << 6U) | 2U);
    const auto packets = decode_pm4(stream);
    const auto draws = packets ? decode_draw_commands(packets.packets, stream)
                               : DrawDecodeResult{{}, "PM4 decode failed", 0U};
    check(static_cast<bool>(draws) && draws.commands.size() == 2U,
          "DRAW_INDX and DRAW_INDX_2 produce metadata without submitting work");
    if (draws && draws.commands.size() == 2U) {
        const auto& dma = draws.commands[0];
        check(dma.has_viz_query_condition && dma.viz_query_condition == 0x105U &&
                  dma.source == DrawSource::dma && dma.primitive_type == 3U &&
                  dma.index_count == 3U && !dma.index_32_bit &&
                  dma.dma_base == 0x00123456U && dma.dma_size == 3U,
              "DMA draw exposes query token, initiator fields, base and size");
        const auto& automatic = draws.commands[1];
        check(!automatic.has_viz_query_condition && automatic.source == DrawSource::auto_index &&
                  automatic.primitive_type == 2U && automatic.index_count == 2U,
              "DRAW_INDX_2 decodes auto-index metadata without a query prefix");

        std::vector<std::uint32_t> registers(0x2200U, 0U);
        const auto applied = apply_draw_command_state(draws.commands, registers);
        check(static_cast<bool>(applied) && registers[0x21fcU] == automatic.initiator &&
                  registers[0x21faU] == 0x00123456U && registers[0x21fbU] == 3U,
              "draw state updates initiator and DMA registers while preserving DMA state "
              "for auto-index");

        std::vector<std::uint32_t> too_small(0x21fcU, 0xccccccccU);
        const auto rejected = apply_draw_command_state(draws.commands, too_small);
        check(!rejected && too_small.back() == 0xccccccccU,
              "out-of-range draw state rejects the command list without partial writes");

        auto forged = draws.commands;
        forged[1].index_count++;
        std::fill(registers.begin(), registers.end(), 0xccccccccU);
        const auto inconsistent = apply_draw_command_state(forged, registers);
        check(!inconsistent && registers[0x21fcU] == 0xccccccccU,
              "inconsistent decoded draw metadata fails before changing register state");
    }

    std::vector<std::uint8_t> truncated;
    append_u32_be(truncated, 0xc0000000U | (0x22U << 8U));
    append_u32_be(truncated, 0U); // Query token, but missing initiator.
    const auto truncated_packets = decode_pm4(truncated);
    const auto missing_initiator = decode_draw_commands(truncated_packets.packets, truncated);
    check(!missing_initiator && missing_initiator.commands.empty(),
          "draw packet missing initiator fails without partial commands");

    std::vector<std::uint8_t> immediate;
    append_u32_be(immediate, 0xc0000000U | (2U << 16U) | (0x36U << 8U));
    append_u32_be(immediate, (1U << 6U) | 3U);
    append_u32_be(immediate, 0x1234U);
    const auto immediate_packets = decode_pm4(immediate);
    const auto unsupported = decode_draw_commands(immediate_packets.packets, immediate);
    check(!unsupported && unsupported.commands.empty(),
          "immediate-index draw reports unsupported semantics explicitly");
}

void test_pm4_sequential_draw_dispatch() {
    using namespace project360::gpu::xenos;
    project360::memory::GuestMemory memory(0x100U);
    const std::array<std::byte, 6> index_bytes{
        std::byte{0U}, std::byte{0U}, std::byte{1U},
        std::byte{0U}, std::byte{2U}, std::byte{0U},
    };
    check(memory.load(0x10U, index_bytes), "PM4 draw-dispatch index fixture loads");
    const std::array<std::uint32_t, 4> alu_constant_words{
        std::bit_cast<std::uint32_t>(4.25F), std::bit_cast<std::uint32_t>(-2.0F),
        std::bit_cast<std::uint32_t>(0.5F), std::bit_cast<std::uint32_t>(9.0F)};
    std::array<std::byte, 16> alu_constant_bytes{};
    for (std::size_t i = 0U; i < alu_constant_words.size(); ++i) {
        const auto value = alu_constant_words[i];
        alu_constant_bytes[i * 4U] = static_cast<std::byte>((value >> 24U) & 0xffU);
        alu_constant_bytes[i * 4U + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
        alu_constant_bytes[i * 4U + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
        alu_constant_bytes[i * 4U + 3U] = static_cast<std::byte>(value & 0xffU);
    }
    check(memory.load(0x60U, alu_constant_bytes),
          "PM4 ALU constant fixture stores big-endian float dwords in guest memory");
    const std::array<std::uint32_t, 4> second_alu_constant_words{
        std::bit_cast<std::uint32_t>(-1.0F), std::bit_cast<std::uint32_t>(2.0F),
        std::bit_cast<std::uint32_t>(3.0F), std::bit_cast<std::uint32_t>(4.0F)};
    std::array<std::byte, 16> second_alu_constant_bytes{};
    for (std::size_t i = 0U; i < second_alu_constant_words.size(); ++i) {
        const auto value = second_alu_constant_words[i];
        second_alu_constant_bytes[i * 4U] = static_cast<std::byte>((value >> 24U) & 0xffU);
        second_alu_constant_bytes[i * 4U + 1U] =
            static_cast<std::byte>((value >> 16U) & 0xffU);
        second_alu_constant_bytes[i * 4U + 2U] =
            static_cast<std::byte>((value >> 8U) & 0xffU);
        second_alu_constant_bytes[i * 4U + 3U] = static_cast<std::byte>(value & 0xffU);
    }
    check(memory.load(0x70U, second_alu_constant_bytes),
          "PM4 second ALU constant fixture stores a distinct float4 in guest memory");

    const auto pack_control_flow = [](std::uint32_t first_word0,
                                      std::uint32_t first_word1,
                                      std::uint32_t second_word0,
                                      std::uint32_t second_word1) {
        return std::array<std::uint32_t, 3>{
            first_word0, first_word1 | (second_word0 << 16U),
            (second_word0 >> 16U) | (second_word1 << 16U)};
    };
    const auto exec_pair = pack_control_flow(1U | (1U << 12U), 1U << 12U,
                                             1U | (1U << 12U), 2U << 12U);
    const std::array<std::uint32_t, 6> guest_shader_dwords{
        exec_pair[0U], exec_pair[1U], exec_pair[2U], 0U, 0U, 0U};
    std::vector<std::uint8_t> guest_shader_bytes;
    for (const auto dword : guest_shader_dwords) append_u32_be(guest_shader_bytes, dword);
    std::vector<std::byte> guest_shader_storage;
    guest_shader_storage.reserve(guest_shader_bytes.size());
    for (const auto byte : guest_shader_bytes) {
        guest_shader_storage.push_back(static_cast<std::byte>(byte));
    }
    check(memory.load(0x80U, guest_shader_storage),
          "PM4 shader-load fixture stores a packed vertex program in guest physical memory");

    std::vector<std::uint8_t> stream;
    append_u32_be(stream, 0xc0000000U | (1U << 16U) | (0x27U << 8U)); // IM_LOAD.
    append_u32_be(stream, 0x80U); // Guest physical shader address, vertex stage.
    append_u32_be(stream, 6U);   // Start 0, six dwords.
    append_u32_be(stream, 0xc0000000U | (2U << 16U) | (0x2fU << 8U)); // LOAD_ALU_CONSTANT.
    append_u32_be(stream, 0x60U); // Physical source address.
    append_u32_be(stream, 36U);  // ALU float4 c9 starts at dword index 36, type 0.
    append_u32_be(stream, 4U);   // Four dwords (one float4 constant).
    append_u32_be(stream, (2U << 16U) | 0x2100U); // VGT_MAX/MIN/INDX_OFFSET.
    append_u32_be(stream, 0x00ffffffU);
    append_u32_be(stream, 0U);
    append_u32_be(stream, 0U);
    append_u32_be(stream, 0xc0000000U | (2U << 16U) | (0x2dU << 8U)); // SET_CONSTANT.
    append_u32_be(stream, (1U << 16U) | 14U); // Fetch slot 7, two dwords per slot.
    append_u32_be(stream, (0x10U << 2U) | 3U); // Base 0x40, vertex-buffer type.
    append_u32_be(stream, 3U << 2U); // Three dwords.
    append_u32_be(stream, 0xc0000000U | (4U << 16U) | (0x2dU << 8U));
    append_u32_be(stream, 28U); // ALU float4 c7.
    append_u32_be(stream, std::bit_cast<std::uint32_t>(1.5F));
    append_u32_be(stream, 0U);
    append_u32_be(stream, 0U);
    append_u32_be(stream, 0U);
    const auto append_dma_draw = [&stream]() {
        append_u32_be(stream, 0xc0000000U | (3U << 16U) | (0x22U << 8U));
        append_u32_be(stream, 0U); // Visibility-query token.
        append_u32_be(stream, (3U << 16U) | 4U); // Three 16-bit DMA triangle indexes.
        append_u32_be(stream, 0x10U);
        append_u32_be(stream, 3U);
    };
    const auto first_draw_word_offset = stream.size() / 4U;
    append_dma_draw();
    append_u32_be(stream, 0x2102U); // Change vertex-index offset before draw two.
    append_u32_be(stream, 1U);
    append_u32_be(stream, 0xc0000000U | (2U << 16U) | (0x2dU << 8U));
    append_u32_be(stream, (1U << 16U) | 14U); // Change fetch slot 7's base.
    append_u32_be(stream, (0x18U << 2U) | 3U);
    append_u32_be(stream, 3U << 2U);
    append_u32_be(stream, 0xc0000000U | (4U << 16U) | (0x2dU << 8U));
    append_u32_be(stream, 28U); // Change ALU float4 c7 for draw two.
    append_u32_be(stream, std::bit_cast<std::uint32_t>(2.5F));
    append_u32_be(stream, 0U);
    append_u32_be(stream, 0U);
    append_u32_be(stream, 0U);
    append_u32_be(stream, 0xc0000000U | (2U << 16U) | (0x2fU << 8U));
    append_u32_be(stream, 0x70U); // Reload c9 from a distinct physical address.
    append_u32_be(stream, 36U);
    append_u32_be(stream, 4U);
    append_u32_be(stream, 0xc0000000U | (7U << 16U) | (0x2bU << 8U)); // IM_LOAD_IMMEDIATE.
    append_u32_be(stream, 1U); // Pixel stage.
    append_u32_be(stream, 6U); // Start 0, six dwords.
    append_u32_be(stream, exec_pair[0U]);
    append_u32_be(stream, exec_pair[1U]);
    append_u32_be(stream, exec_pair[2U]);
    append_u32_be(stream, 1U);
    append_u32_be(stream, 0U);
    append_u32_be(stream, 0U);
    const auto second_draw_word_offset = stream.size() / 4U;
    append_dma_draw();

    std::vector<std::uint32_t> initial_registers(0x5000U, 0U);
    const auto dispatched = process_pm4_draw_stream(stream, initial_registers, memory);
    if (!dispatched) {
        std::cerr << "PM4 shader-load integration diagnostic at word "
                  << dispatched.error_word_offset << ": " << dispatched.error << '\n';
    }
    check(dispatched && dispatched.draws.size() == 2U,
          "PM4 stream prepares both draws before exposing their state snapshots");
    if (!dispatched || dispatched.draws.size() != 2U) return;
    constexpr std::array<std::uint8_t, 1> required_fetch_slots{7U};
    constexpr std::array<std::uint8_t, 2> required_alu_constants{7U, 9U};
    const auto first_fetch_bank = decode_vertex_fetch_descriptor_bank(
        dispatched.draws[0].register_snapshot, required_fetch_slots);
    const auto second_fetch_bank = decode_vertex_fetch_descriptor_bank(
        dispatched.draws[1].register_snapshot, required_fetch_slots);
    const auto first_alu_bank = decode_shader_alu_float_constant_bank(
        dispatched.draws[0].register_snapshot, required_alu_constants);
    const auto second_alu_bank = decode_shader_alu_float_constant_bank(
        dispatched.draws[1].register_snapshot, required_alu_constants);
    check(dispatched.draws[0].command.header_word_offset == first_draw_word_offset &&
              dispatched.draws[1].command.header_word_offset == second_draw_word_offset &&
              dispatched.draws[0].geometry.indices ==
                  std::vector<std::uint32_t>{0U, 1U, 2U} &&
              dispatched.draws[1].geometry.indices ==
                  std::vector<std::uint32_t>{1U, 2U, 3U} &&
              dispatched.final_registers[register_index::vgt_indx_offset] == 1U &&
              first_fetch_bank && second_fetch_bank &&
              first_fetch_bank.descriptors[7U].base_address_bytes == 0x40U &&
              second_fetch_bank.descriptors[7U].base_address_bytes == 0x60U &&
              first_fetch_bank.descriptors[7U].size_dwords == 3U &&
              second_fetch_bank.descriptors[7U].size_dwords == 3U && first_alu_bank &&
              second_alu_bank && first_alu_bank.constants[7U][0] == 1.5F &&
              second_alu_bank.constants[7U][0] == 2.5F &&
              first_alu_bank.constants[9U] ==
                  std::array<float, 4>{4.25F, -2.0F, 0.5F, 9.0F} &&
              second_alu_bank.constants[9U] ==
                  std::array<float, 4>{-1.0F, 2.0F, 3.0F, 4.0F} &&
              dispatched.draws[0].vertex_shader &&
              dispatched.draws[0].vertex_shader->stage == ShaderStage::vertex &&
              dispatched.draws[0].vertex_shader->source_address == 0x80U &&
              dispatched.draws[0].vertex_shader->records.size() == 2U &&
              !dispatched.draws[0].pixel_shader &&
              dispatched.draws[1].vertex_shader == dispatched.draws[0].vertex_shader &&
              dispatched.draws[1].pixel_shader &&
              dispatched.draws[1].pixel_shader->stage == ShaderStage::pixel &&
              dispatched.draws[1].pixel_shader->source_address == 0U &&
              dispatched.draws[1].pixel_shader->records.size() == 2U &&
              dispatched.draws[0].register_snapshot[register_index::vgt_indx_offset] == 0U &&
              dispatched.draws[1].register_snapshot[register_index::vgt_indx_offset] == 1U,
          "PM4 dispatch retains independent per-draw fetch and ALU constant state");

    if (dispatched.draws[0].vertex_shader) {
        const auto parsed = decode_shader_exec_program(
            dispatched.draws[0].vertex_shader->records, 1U);
        ShaderAluFloatConstantBank constants{};
        const auto executed = parsed
                                  ? execute_shader_exec_sequence(
                                        parsed.control_flow,
                                        dispatched.draws[0].vertex_shader->records, {}, memory,
                                        {}, {}, constants)
                                  : ShaderExecSequenceResult{};
        check(parsed && executed && executed.ended_shader &&
                  executed.instructions_executed == 2U,
              "PM4-loaded guest vertex microcode reaches the CPU shader reference executor");
    } else {
        check(false, "PM4-loaded guest vertex microcode reaches the CPU shader reference executor");
    }

    auto unsupported_stream = stream;
    append_u32_be(unsupported_stream, 0xc0000000U | (0x7fU << 8U));
    append_u32_be(unsupported_stream, 0U);
    const auto rejected =
        process_pm4_draw_stream(unsupported_stream, initial_registers, memory);
    check(!rejected && rejected.draws.empty() && rejected.final_registers.empty() &&
              rejected.error_word_offset == stream.size() / 4U,
          "PM4 draw dispatch rejects unsupported later commands without partial output");

    std::vector<std::uint8_t> incomplete_shader_load;
    append_u32_be(incomplete_shader_load,
                  0xc0000000U | (1U << 16U) | (0x27U << 8U)); // IM_LOAD.
    append_u32_be(incomplete_shader_load, 0x80U);
    append_u32_be(incomplete_shader_load, 5U); // Not a complete three-dword record count.
    const auto rejected_incomplete_shader =
        process_pm4_draw_stream(incomplete_shader_load, initial_registers, memory);
    check(!rejected_incomplete_shader && rejected_incomplete_shader.draws.empty() &&
              rejected_incomplete_shader.error.find("complete three-dword records") !=
                  std::string::npos,
          "PM4_IM_LOAD rejects partial microcode records transactionally");

    std::vector<std::uint8_t> truncated_immediate_shader;
    append_u32_be(truncated_immediate_shader,
                  0xc0000000U | (6U << 16U) | (0x2bU << 8U)); // Seven payload dwords.
    append_u32_be(truncated_immediate_shader, 1U); // Pixel stage.
    append_u32_be(truncated_immediate_shader, 6U); // Declares six code dwords.
    for (std::size_t i = 0U; i < 5U; ++i) append_u32_be(truncated_immediate_shader, 0U);
    const auto rejected_truncated_immediate =
        process_pm4_draw_stream(truncated_immediate_shader, initial_registers, memory);
    check(!rejected_truncated_immediate && rejected_truncated_immediate.draws.empty() &&
              rejected_truncated_immediate.error.find("payload must match") != std::string::npos,
          "PM4_IM_LOAD_IMMEDIATE validates declared dwords against packet payload bounds");

    std::vector<std::uint8_t> predicated_shader_load_skipped;
    append_u32_be(predicated_shader_load_skipped,
                  0xc0000000U | (1U << 16U) | (0x50U << 8U)); // SET_BIN_MASK.
    append_u32_be(predicated_shader_load_skipped, 0U);
    append_u32_be(predicated_shader_load_skipped, 1U);
    append_u32_be(predicated_shader_load_skipped,
                  0xc0000000U | (1U << 16U) | (0x51U << 8U)); // SET_BIN_SELECT.
    append_u32_be(predicated_shader_load_skipped, 0U);
    append_u32_be(predicated_shader_load_skipped, 0U);
    append_u32_be(predicated_shader_load_skipped, 0xc0000001U | (0x27U << 8U));
    append_u32_be(predicated_shader_load_skipped, 0U); // Invalid IM_LOAD body, skipped by bin test.
    const auto skipped_bin_shader_load = process_pm4_draw_stream(
        predicated_shader_load_skipped, initial_registers, memory);
    check(skipped_bin_shader_load && skipped_bin_shader_load.draws.empty() &&
              skipped_bin_shader_load.final_registers == initial_registers,
          "PM4 bin mask skips a predicated shader packet when no selected bin matches");

    std::vector<std::uint8_t> predicated_shader_load_selected;
    append_u32_be(predicated_shader_load_selected,
                  0xc0000000U | (0x60U << 8U)); // SET_BIN_MASK_LO.
    append_u32_be(predicated_shader_load_selected, 1U);
    append_u32_be(predicated_shader_load_selected,
                  0xc0000000U | (0x61U << 8U)); // SET_BIN_MASK_HI.
    append_u32_be(predicated_shader_load_selected, 0x80000000U);
    append_u32_be(predicated_shader_load_selected,
                  0xc0000000U | (0x62U << 8U)); // SET_BIN_SELECT_LO.
    append_u32_be(predicated_shader_load_selected, 1U);
    append_u32_be(predicated_shader_load_selected,
                  0xc0000000U | (0x63U << 8U)); // SET_BIN_SELECT_HI.
    append_u32_be(predicated_shader_load_selected, 0x80000000U);
    append_u32_be(predicated_shader_load_selected, 0xc0000001U | (0x27U << 8U));
    append_u32_be(predicated_shader_load_selected, 0U);
    const auto selected_bin_shader_load = process_pm4_draw_stream(
        predicated_shader_load_selected, initial_registers, memory);
    check(!selected_bin_shader_load && selected_bin_shader_load.draws.empty() &&
              selected_bin_shader_load.error.find("requires an address") != std::string::npos,
          "PM4 bin-mask low/high state selects predicated shader packets transactionally");

    std::vector<std::uint8_t> out_of_bounds_constant_load;
    append_u32_be(out_of_bounds_constant_load,
                  0xc0000000U | (2U << 16U) | (0x2fU << 8U));
    append_u32_be(out_of_bounds_constant_load, 0xfcU);
    append_u32_be(out_of_bounds_constant_load, 9U);
    append_u32_be(out_of_bounds_constant_load, 4U);
    const auto rejected_constant_load = process_pm4_draw_stream(
        out_of_bounds_constant_load, initial_registers, memory);
    check(!rejected_constant_load && rejected_constant_load.draws.empty() &&
              rejected_constant_load.final_registers.empty() &&
              rejected_constant_load.error.find("guest physical memory") != std::string::npos,
          "PM4_LOAD_ALU_CONSTANT rejects out-of-range guest memory transactionally");

    std::vector<std::uint8_t> unsupported_constant_bank;
    append_u32_be(unsupported_constant_bank,
                  0xc0000000U | (2U << 16U) | (0x2fU << 8U));
    append_u32_be(unsupported_constant_bank, 0x60U);
    append_u32_be(unsupported_constant_bank, (5U << 16U) | 9U);
    append_u32_be(unsupported_constant_bank, 4U);
    const auto rejected_constant_bank = process_pm4_draw_stream(
        unsupported_constant_bank, initial_registers, memory);
    check(!rejected_constant_bank && rejected_constant_bank.final_registers.empty() &&
              rejected_constant_bank.error.find("unsupported constant type") != std::string::npos,
          "PM4_LOAD_ALU_CONSTANT rejects unsupported constant banks explicitly");
}

void test_pm4_load_constant_banks_from_memory() {
    using namespace project360::gpu::xenos;
    project360::memory::GuestMemory memory(64U);
    const std::array<std::uint32_t, 4> source_words{
        std::bit_cast<std::uint32_t>(1.25F), std::bit_cast<std::uint32_t>(-3.0F),
        std::bit_cast<std::uint32_t>(0.75F), std::bit_cast<std::uint32_t>(8.0F)};
    std::array<std::byte, 16> source_bytes{};
    for (std::size_t i = 0U; i < source_words.size(); ++i) {
        const auto value = source_words[i];
        source_bytes[i * 4U] = static_cast<std::byte>((value >> 24U) & 0xffU);
        source_bytes[i * 4U + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
        source_bytes[i * 4U + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
        source_bytes[i * 4U + 3U] = static_cast<std::byte>(value & 0xffU);
    }
    check(memory.load(0U, source_bytes), "PM4 bank-load fixture initializes physical source memory");

    std::vector<std::uint8_t> stream;
    const auto append_load = [&stream](std::uint32_t type, std::uint32_t index,
                                       std::uint32_t size_dwords) {
        append_u32_be(stream, 0xc0000000U | (2U << 16U) | (0x2fU << 8U));
        append_u32_be(stream, 0U);
        append_u32_be(stream, (type << 16U) | index);
        append_u32_be(stream, size_dwords);
    };
    append_load(0U, 36U, 4U);    // ALU c9, float4.
    append_load(1U, 19U, 1U);    // Fetch bank.
    append_load(2U, 5U, 1U);     // Boolean bank.
    append_load(3U, 2U, 1U);     // Loop bank.
    append_load(4U, 0x100U, 1U); // Direct register bank.

    const std::vector<std::uint32_t> initial_registers(0x5000U, 0U);
    const auto loaded = process_pm4_draw_stream(stream, initial_registers, memory);
    const std::array<std::uint8_t, 1> required_constant{9U};
    const auto alu_bank = loaded ? decode_shader_alu_float_constant_bank(
                                       loaded.final_registers, required_constant)
                                 : ShaderAluFloatConstantBankResult{};
    check(loaded && loaded.draws.empty() && alu_bank &&
              alu_bank.constants[9U] == std::array<float, 4>{1.25F, -3.0F, 0.75F, 8.0F} &&
              loaded.final_registers[0x4813U] == source_words[0U] &&
              loaded.final_registers[0x4905U] == source_words[0U] &&
              loaded.final_registers[0x490aU] == source_words[0U] &&
              loaded.final_registers[0x2100U] == source_words[0U],
          "PM4_LOAD_ALU_CONSTANT maps ALU, fetch, boolean, loop and direct register banks");
}

void test_xenos_scissor_register_resolution() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint32_t> registers(register_index::pa_sc_window_scissor_br + 1U, 0U);
    registers[register_index::pa_sc_screen_scissor_tl] =
        (0x7ffeU) | (1U << 16U); // (-2, 1) signed 15-bit screen origin.
    registers[register_index::pa_sc_screen_scissor_br] = 15U | (14U << 16U);
    registers[register_index::pa_sc_window_offset] = 1U | (0x7fffU << 16U); // (+1, -1).
    registers[register_index::pa_sc_window_scissor_tl] = 2U | (3U << 16U);
    registers[register_index::pa_sc_window_scissor_br] = 14U | (15U << 16U);

    const auto offset_scissor = read_xenos_scissor_rectangle(registers, 12U, 10U);
    check(offset_scissor &&
              offset_scissor.rectangle == XenosScissorRectangle{3U, 2U, 9U, 8U},
          "Xenos scissor intersects screen bounds, applies signed window offsets and clamps target");

    registers[register_index::pa_sc_window_scissor_tl] |= 1U << 31U;
    const auto offset_disabled_scissor = read_xenos_scissor_rectangle(registers, 12U, 10U);
    check(offset_disabled_scissor &&
              offset_disabled_scissor.rectangle == XenosScissorRectangle{2U, 3U, 10U, 7U},
          "Xenos window-scissor offset-disable bit preserves unshifted bounds");

    const auto truncated = read_xenos_scissor_rectangle(
        std::span<const std::uint32_t>(registers.data(), register_index::pa_sc_window_scissor_br),
        12U, 10U);
    check(!truncated && truncated.rectangle.width == 0U,
          "truncated Xenos scissor register files fail without a partial rectangle");
}

void test_xenos_viewport_register_resolution() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint32_t> registers(register_index::pa_su_vtx_cntl + 1U, 0U);
    registers[register_index::pa_cl_clip_cntl] = 1U << 19U;
    registers[register_index::pa_cl_vte_cntl] = 0x0fU;
    registers[register_index::pa_cl_vport_xscale] = std::bit_cast<std::uint32_t>(6.0F);
    registers[register_index::pa_cl_vport_xoffset] = std::bit_cast<std::uint32_t>(8.0F);
    registers[register_index::pa_cl_vport_yscale] = std::bit_cast<std::uint32_t>(-6.0F);
    registers[register_index::pa_cl_vport_yoffset] = std::bit_cast<std::uint32_t>(8.5F);
    registers[register_index::pa_su_sc_mode_cntl] = 1U << 16U;
    registers[register_index::pa_sc_window_offset] = 1U | (0x7ffeU << 16U);

    const auto d3d_pixel_center = read_xenos_viewport(registers);
    check(d3d_pixel_center &&
              d3d_pixel_center.viewport == XenosViewport{3.5F, 1.0F, 12.0F, 12.0F,
                                                          false, true},
          "Xenos viewport applies signed scale, enabled window offset and D3D half-pixel shift");

    registers[register_index::pa_su_vtx_cntl] = 1U;
    const auto half_pixel_center = read_xenos_viewport(registers);
    check(half_pixel_center &&
              half_pixel_center.viewport == XenosViewport{3.0F, 0.5F, 12.0F, 12.0F,
                                                           false, true},
          "Xenos OGL pixel center omits the D3D half-pixel shift");

    registers[register_index::pa_cl_vte_cntl] = 0x3fU;
    registers[register_index::pa_cl_vport_zscale] = std::bit_cast<std::uint32_t>(0.25F);
    registers[register_index::pa_cl_vport_zoffset] = std::bit_cast<std::uint32_t>(0.25F);
    const auto d3d_depth_range = read_xenos_viewport(registers);
    check(d3d_depth_range && d3d_depth_range.viewport.min_depth == 0.25F &&
              d3d_depth_range.viewport.max_depth == 0.5F &&
              !d3d_depth_range.viewport.flip_z &&
              !d3d_depth_range.viewport.remap_z_from_gl_clip_space,
          "Xenos D3D viewport maps enabled Z scale and offset to Vulkan depth range");

    registers[register_index::pa_cl_clip_cntl] = 0U;
    const auto symmetric_gl_clip = read_xenos_viewport(registers);
    check(symmetric_gl_clip && symmetric_gl_clip.viewport.remap_z_from_gl_clip_space &&
              symmetric_gl_clip.viewport.x == 3.0F &&
              symmetric_gl_clip.viewport.y == 0.5F &&
              symmetric_gl_clip.viewport.width == 12.0F &&
              symmetric_gl_clip.viewport.height == 12.0F &&
              symmetric_gl_clip.viewport.min_depth == 0.0F &&
              symmetric_gl_clip.viewport.max_depth == 0.5F,
          "Xenos symmetric depth mode remaps -W..W and applies its transformed depth range");

    registers[register_index::pa_cl_clip_cntl] = 1U << 19U;
    registers[register_index::pa_cl_vport_zscale] = std::bit_cast<std::uint32_t>(-0.25F);
    const auto reverse_z = read_xenos_viewport(registers);
    check(reverse_z && reverse_z.viewport.flip_z && reverse_z.viewport.min_depth == 0.0F &&
              reverse_z.viewport.max_depth == 0.25F,
          "Xenos reverse depth range swaps Vulkan endpoints and flips clip-space Z");

    registers[register_index::pa_cl_vte_cntl] = 0x0fU;
    registers[register_index::pa_cl_clip_cntl] |= 1U << 16U;
    check(!read_xenos_viewport(registers),
          "Xenos viewport rejects clipping-disabled raster state");
    registers[register_index::pa_cl_clip_cntl] = (1U << 19U) | 1U;
    check(!read_xenos_viewport(registers),
          "Xenos viewport rejects unsupported user clip planes");

    registers[register_index::pa_cl_clip_cntl] = 1U << 19U;
    registers[register_index::pa_cl_vport_zscale] =
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    registers[register_index::pa_cl_vte_cntl] |= 1U << 4U;
    check(!read_xenos_viewport(registers),
          "Xenos viewport rejects non-finite depth transform values");
    registers[register_index::pa_cl_vte_cntl] = 0x0fU;
    registers[register_index::pa_cl_vport_xscale] =
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
    check(!read_xenos_viewport(registers),
          "Xenos viewport rejects non-finite scale values");
}

void test_dma_index_fetch() {
    using namespace project360::gpu::xenos;
    project360::memory::GuestMemory memory(0x4000U);
    const std::array<std::byte, 4> index16_bytes{
        std::byte{0x12}, std::byte{0x34}, std::byte{0xab}, std::byte{0xcd}};
    check(memory.load(8U, index16_bytes), "16-bit DMA fixture loads into physical memory");
    check(memory.map_page(0x1000U, 0U, project360::memory::GuestPageAccess::read),
          "CPU virtual mapping can coexist with GPU physical reads");

    DrawCommand draw16{};
    draw16.source = DrawSource::dma;
    draw16.index_count = 2U;
    draw16.dma_base = 8U;
    draw16.dma_size = (1U << 30U) | 2U; // 8-in-16, two index elements.
    const auto swapped16 = fetch_draw_indices(draw16, memory);
    check(static_cast<bool>(swapped16) && swapped16.indices.size() == 2U &&
              swapped16.indices[0] == 0x1234U && swapped16.indices[1] == 0xabcdU,
          "16-bit DMA fetch applies the supported 8-in-16 byte swap");

    draw16.dma_size = 2U; // No swap.
    const auto native16 = fetch_draw_indices(draw16, memory);
    check(static_cast<bool>(native16) && native16.indices[0] == 0x3412U &&
              native16.indices[1] == 0xcdabU,
          "16-bit DMA fetch preserves bytes in no-swap mode");

    const std::array<std::byte, 4> index32_bytes{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    check(memory.load(16U, index32_bytes), "32-bit DMA fixture loads into physical memory");
    constexpr std::array<std::uint32_t, 4> expected_by_mode{
        0x44332211U, 0x33441122U, 0x11223344U, 0x22114433U};
    DrawCommand draw32{};
    draw32.source = DrawSource::dma;
    draw32.index_32_bit = true;
    draw32.index_count = 1U;
    draw32.dma_base = 16U;
    for (std::uint32_t mode = 0U; mode < 4U; ++mode) {
        draw32.dma_size = (mode << 30U) | 1U;
        const auto fetched = fetch_draw_indices(draw32, memory);
        check(static_cast<bool>(fetched) && fetched.indices.size() == 1U &&
                  fetched.indices[0] == expected_by_mode[mode],
              "32-bit DMA fetch matches each documented byte-swap mode");
    }

    draw16.dma_size = (3U << 30U) | 2U;
    const auto ambiguous16 = fetch_draw_indices(draw16, memory);
    check(!ambiguous16 && ambiguous16.indices.empty(),
          "ambiguous 32-bit swap mode on 16-bit indexes is rejected explicitly");

    draw16.dma_size = 1U;
    const auto short_dma = fetch_draw_indices(draw16, memory);
    check(!short_dma && short_dma.indices.empty(),
          "DMA index count cannot exceed its declared element capacity");

    draw32.dma_base = 0x4000U;
    draw32.dma_size = 1U;
    const auto out_of_bounds = fetch_draw_indices(draw32, memory);
    check(!out_of_bounds && out_of_bounds.indices.empty(),
          "DMA index access outside physical guest memory fails without partial output");

    DrawCommand automatic{};
    automatic.source = DrawSource::auto_index;
    automatic.index_count = 4U;
    const auto generated = fetch_draw_indices(automatic, memory);
    check(static_cast<bool>(generated) && generated.indices.size() == 4U &&
              generated.indices[0] == 0U && generated.indices[3] == 3U,
          "auto-index source generates the guest sequential index range");
}

void test_primitive_assembly() {
    using namespace project360::gpu::xenos;
    DrawCommand command{};
    command.source = DrawSource::auto_index;
    command.primitive_type = 5U; // Triangle fan.
    command.index_count = 4U;
    const std::array<std::uint32_t, 4> fan{10U, 11U, 12U, 13U};
    const auto fan_result = assemble_draw_primitives(command, fan);
    check(static_cast<bool>(fan_result) &&
              fan_result.topology == PrimitiveTopology::triangles &&
              fan_result.indices == std::vector<std::uint32_t>{11U, 12U, 10U, 12U, 13U,
                                                               10U} &&
              fan_result.ranges.size() == 1U && fan_result.ranges[0].index_count == 6U,
          "triangle fan becomes indexed triangles with a bounded draw range");

    command.primitive_type = 0x0cU; // Line loop.
    const auto loop_result = assemble_draw_primitives(command, fan);
    check(static_cast<bool>(loop_result) && loop_result.topology == PrimitiveTopology::lines &&
              loop_result.indices == std::vector<std::uint32_t>{10U, 11U, 11U, 12U, 12U,
                                                                13U, 13U, 10U},
          "line loop closes each sequence as independent line segments");

    command.primitive_type = 0x0dU; // Quad list.
    command.index_count = 5U;
    const std::array<std::uint32_t, 5> quad{20U, 21U, 22U, 23U, 24U};
    const auto quad_result = assemble_draw_primitives(command, quad);
    check(static_cast<bool>(quad_result) &&
              quad_result.topology == PrimitiveTopology::triangles &&
              quad_result.indices == std::vector<std::uint32_t>{20U, 21U, 22U, 20U, 22U,
                                                                 23U},
          "quad list uses two triangles and ignores an incomplete trailing quad");

    command.primitive_type = 6U; // Triangle strip.
    command.index_32_bit = true;
    command.index_count = 7U;
    const std::array<std::uint32_t, 7> split_strip{1U, 2U, 3U, 0x120000ffU,
                                                   4U, 5U, 6U};
    const auto strip_result = assemble_draw_primitives(
        command, split_strip, {.restart_enabled = true, .restart_index = 0x000000ffU});
    check(static_cast<bool>(strip_result) &&
              strip_result.topology == PrimitiveTopology::triangle_strip &&
              strip_result.indices == std::vector<std::uint32_t>{1U, 2U, 3U, 4U, 5U, 6U} &&
              strip_result.ranges.size() == 2U && strip_result.ranges[0].index_count == 3U &&
              strip_result.ranges[1].first_index == 3U,
          "32-bit primitive restart splits ranges using the low 24-bit reset value");

    command.primitive_type = 5U;
    command.index_32_bit = false;
    command.index_count = 7U;
    const std::array<std::uint32_t, 7> split_fan{1U, 2U, 3U, 0xffU, 4U, 5U, 6U};
    const auto restart_fan = assemble_draw_primitives(
        command, split_fan, {.restart_enabled = true, .restart_index = 0xffU});
    check(static_cast<bool>(restart_fan) && restart_fan.ranges.size() == 2U &&
              restart_fan.indices == std::vector<std::uint32_t>{2U, 3U, 1U, 5U, 6U, 4U},
          "16-bit primitive restart splits triangle fans before fan expansion");

    command.primitive_type = 4U;
    const auto unsupported_restart = assemble_draw_primitives(
        command, split_fan, {.restart_enabled = true, .restart_index = 0xffU});
    check(!unsupported_restart && unsupported_restart.indices.empty(),
          "restart on a topology without modeled Xenos semantics fails explicitly");
    command.primitive_type = 0x10U;
    const auto unsupported_type = assemble_draw_primitives(command, split_fan);
    check(!unsupported_type && unsupported_type.indices.empty(),
          "unsupported Xenos topology is rejected without generated indices");
    command.primitive_type = 4U;
    command.index_count = 2U;
    const auto count_mismatch = assemble_draw_primitives(command, split_fan);
    check(!count_mismatch && count_mismatch.indices.empty(),
          "primitive assembly rejects an index span inconsistent with draw metadata");
}

void test_vertex_shader_index_transform() {
    using namespace project360::gpu::xenos;
    check(transform_vertex_shader_index(0xab000005U, {1U, 0U, 0x00ffffffU}) == 6U,
          "vertex shader index uses the low 24 bits after adding the draw offset");
    check(transform_vertex_shader_index(0x00ffffffU, {2U, 0U, 0x00ffffffU}) == 1U,
          "vertex shader index offset wraps within the 24-bit guest index range");
    check(transform_vertex_shader_index(5U, {2U, 10U, 100U}) == 10U,
          "vertex shader index is clamped to the minimum after offsetting");
    check(transform_vertex_shader_index(150U, {3U, 10U, 100U}) == 100U,
          "vertex shader index is clamped to the maximum after offsetting");
    std::vector<std::uint32_t> registers(register_index::vgt_indx_offset + 1U, 0U);
    registers[register_index::vgt_max_vtx_indx] = 0xff000064U;
    registers[register_index::vgt_min_vtx_indx] = 0xff00000aU;
    registers[register_index::vgt_indx_offset] = 0xff000003U;
    const auto register_transform = read_vertex_index_transform(registers);
    check(register_transform && register_transform.transform.offset == 3U &&
              register_transform.transform.minimum == 10U &&
              register_transform.transform.maximum == 100U &&
              transform_vertex_shader_index(150U, register_transform.transform) == 100U,
          "vertex index transform reads the named low-24-bit Xenos register fields");
    check(!read_vertex_index_transform(std::span<const std::uint32_t>(registers.data(), 8U)),
          "vertex index transform rejects register snapshots that omit the mapped state");

    project360::memory::GuestMemory memory(0x1000U);
    const std::array<std::byte, 6> triangle_index_bytes{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x00}, std::byte{0x02}, std::byte{0x00}};
    check(memory.load(0x100U, triangle_index_bytes),
          "draw geometry fixture loads indexed triangle data");
    DrawCommand triangle{};
    triangle.source = DrawSource::dma;
    triangle.index_count = 3U;
    triangle.primitive_type = 4U;
    triangle.dma_base = 0x100U;
    triangle.dma_size = 3U;
    registers[register_index::vgt_indx_offset] = 2U;
    registers[register_index::vgt_min_vtx_indx] = 0U;
    registers[register_index::vgt_max_vtx_indx] = 3U;
    const auto geometry = prepare_draw_geometry(triangle, memory, registers);
    check(geometry && geometry.topology == PrimitiveTopology::triangles &&
              geometry.indices == std::vector<std::uint32_t>{2U, 3U, 3U} &&
              geometry.ranges.size() == 1U && geometry.ranges[0].first_index == 0U &&
              geometry.ranges[0].index_count == 3U,
          "draw preparation fetches indexes, assembles topology and applies mapped bounds");

    const std::array<std::byte, 8> restart_index_bytes{
        std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff},
        std::byte{0x01}, std::byte{0x00}, std::byte{0x02}, std::byte{0x00}};
    check(memory.load(0x120U, restart_index_bytes),
          "draw geometry fixture loads primitive-restart data");
    DrawCommand strip{};
    strip.source = DrawSource::dma;
    strip.index_count = 4U;
    strip.primitive_type = 3U;
    strip.dma_base = 0x120U;
    strip.dma_size = 4U;
    registers[register_index::vgt_indx_offset] = 1U;
    registers[register_index::vgt_max_vtx_indx] = 0x00ffffffU;
    const auto restarted = prepare_draw_geometry(strip, memory, registers, {true, 0xffffU});
    check(restarted && restarted.topology == PrimitiveTopology::line_strip &&
              restarted.indices == std::vector<std::uint32_t>{1U, 2U, 3U} &&
              restarted.ranges.size() == 2U && restarted.ranges[0].first_index == 0U &&
              restarted.ranges[0].index_count == 1U &&
              restarted.ranges[1].first_index == 1U &&
              restarted.ranges[1].index_count == 2U,
          "draw preparation recognizes restart indexes before applying vertex offsets");
}

void test_vertex_fetch_descriptors_and_memory() {
    using namespace project360::gpu::xenos;
    project360::memory::GuestMemory memory(0x1000U);
    const std::array<std::byte, 12> bytes{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
        std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88},
        std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}};
    check(memory.load(8U, bytes), "vertex-fetch fixture loads into physical guest memory");

    const auto descriptor = decode_vertex_fetch_descriptor((2U << 2U) | 3U, 3U << 2U);
    check(static_cast<bool>(descriptor) && descriptor.descriptor.base_address_bytes == 8U &&
              descriptor.descriptor.size_dwords == 3U &&
              descriptor.descriptor.endian == VertexFetchEndian::none,
          "vertex-fetch constant decodes dword address, size and endian fields");
    const auto range = read_vertex_fetch_words(descriptor.descriptor, memory, 1U, 2U);
    check(static_cast<bool>(range) &&
              range.words == std::vector<std::uint32_t>{0x88776655U, 0xccbbaa99U},
          "vertex fetch reads only the requested bounded physical dword range");
    const auto past_descriptor = read_vertex_fetch_words(descriptor.descriptor, memory, 2U, 2U);
    check(!past_descriptor && past_descriptor.words.empty(),
          "vertex fetch rejects ranges beyond descriptor size");

    constexpr std::array<std::uint32_t, 4> expected{0x44332211U, 0x33441122U,
                                                     0x11223344U, 0x22114433U};
    for (std::uint32_t mode = 0U; mode < 4U; ++mode) {
        const auto mode_descriptor = decode_vertex_fetch_descriptor(
            (2U << 2U) | 3U, (1U << 2U) | mode);
        const auto word = read_vertex_fetch_words(mode_descriptor.descriptor, memory, 0U, 1U);
        check(static_cast<bool>(word) && word.words.size() == 1U &&
                  word.words[0] == expected[mode],
              "vertex fetch applies each Xenos 32-bit endian mode");
    }

    const auto texture_descriptor = decode_vertex_fetch_descriptor((2U << 2U) | 2U, 4U);
    const auto invalid_descriptor = decode_vertex_fetch_descriptor((2U << 2U) | 1U, 4U);
    const auto reserved_bits = decode_vertex_fetch_descriptor((2U << 2U) | 3U, 0x04000000U);
    check(!texture_descriptor && !invalid_descriptor && !reserved_bits,
          "non-vertex kinds and reserved descriptor bits are rejected explicitly");

    const auto out_of_bounds = decode_vertex_fetch_descriptor((0x3feU << 2U) | 3U, 3U << 2U);
    const auto inaccessible = read_vertex_fetch_words(out_of_bounds.descriptor, memory, 0U, 3U);
    check(!inaccessible && inaccessible.words.empty(),
          "vertex fetch rejects physical memory ranges past guest backing storage");
}

void test_texture_fetch_constant_metadata() {
    using namespace project360::gpu::xenos;
    const std::array<std::uint32_t, 6> words{
        0x80000000U | (0x12U << 22U) | (5U << 16U) | (6U << 13U) | (3U << 10U) |
            (2U << 8U) | (1U << 6U) | (3U << 4U) | (2U << 2U) | 2U,
        (0xabcdeU << 12U) | (1U << 11U) | (1U << 10U) | (2U << 8U) | (3U << 6U) |
            0x25U,
        63U | (31U << 13U) | (3U << 26U),
        (1U << 31U) | (6U << 28U) | (5U << 25U) | (3U << 23U) | (1U << 21U) |
            (2U << 19U) | (0x3dU << 13U) | (0xabcU << 1U) | 1U,
        (3U << 27U) | (0x1eU << 22U) | (0x3feU << 12U) | (1U << 11U) |
            (1U << 10U) | (9U << 6U) | (3U << 2U) | 3U,
        (0x23456U << 12U) | (1U << 11U) | (1U << 9U) | (0xdU << 5U) |
            (2U << 3U) | (1U << 2U) | 2U,
    };
    const auto decoded = decode_texture_fetch_constant(words);
    check(static_cast<bool>(decoded), "texture fetch constant accepts the texture type tag");
    check(decoded.constant.raw_words == words,
          "texture fetch constant preserves all six original dwords");
    check(decoded.constant.sign_modes == std::array<std::uint8_t, 4>{2U, 3U, 1U, 2U},
          "texture fetch constant decodes per-component sign modes");
    check(decoded.constant.clamp_modes == std::array<std::uint8_t, 3>{3U, 6U, 5U},
          "texture fetch constant decodes per-axis clamp modes");
    check(decoded.constant.pitch == 0x12U && decoded.constant.tiled,
          "texture fetch constant decodes pitch and tiling");
    check(decoded.constant.format == 0x25U && decoded.constant.endian == 3U &&
              decoded.constant.request_size == 2U && decoded.constant.stacked &&
              decoded.constant.nearest_clamp_policy &&
              decoded.constant.base_address_bytes == 0xabcde000U,
          "texture fetch constant decodes storage fields and page-based address");
    check(decoded.constant.dimension == TextureDataDimension::two_d_or_stacked &&
              decoded.constant.width == 64U && decoded.constant.height == 32U &&
              decoded.constant.depth_or_layers == 4U,
          "2D texture dimensions decode their minus-one fields");
    check(decoded.constant.number_format == 1U && decoded.constant.component_swizzle == 0xabcU &&
              decoded.constant.exponent_adjust == -3 && decoded.constant.magnification_filter == 2U &&
              decoded.constant.minification_filter == 1U && decoded.constant.mip_filter == 3U &&
              decoded.constant.anisotropic_filter == 5U && decoded.constant.arbitrary_filter == 6U &&
              decoded.constant.border_size,
          "texture fetch constant decodes numeric format, swizzle and filter metadata");
    check(decoded.constant.mip_min_level == 3U && decoded.constant.mip_max_level == 9U &&
              decoded.constant.lod_bias == -2 &&
              decoded.constant.gradient_exponent_adjust_h == -2 &&
              decoded.constant.gradient_exponent_adjust_v == 3 &&
              decoded.constant.magnification_anisotropic_walk &&
              decoded.constant.minification_anisotropic_walk &&
              decoded.constant.volume_magnification_filter &&
              decoded.constant.volume_minification_filter,
          "texture fetch constant decodes signed LOD and mip policy fields");
    check(decoded.constant.border_color == 2U && decoded.constant.force_bc_w_to_max &&
              decoded.constant.tri_clamp == 2U && decoded.constant.anisotropic_bias == -3 &&
              decoded.constant.packed_mips &&
              decoded.constant.mip_address_bytes == 0x23456000U,
          "texture fetch constant decodes border and mip address fields");

    auto size_words = words;
    size_words[5] &= ~(0x3U << 9U);
    size_words[5] |= 0U << 9U;
    size_words[2] = 16U;
    const auto one_d = decode_texture_fetch_constant(size_words);
    size_words[5] = (size_words[5] & ~(0x3U << 9U)) | (2U << 9U);
    size_words[2] = 7U | (9U << 11U) | (2U << 22U);
    const auto three_d = decode_texture_fetch_constant(size_words);
    size_words[5] = (size_words[5] & ~(0x3U << 9U)) | (3U << 9U);
    size_words[2] = 0x1fffU | (0x1fffU << 13U) | (0x3fU << 26U);
    const auto cube = decode_texture_fetch_constant(size_words);
    check(one_d && one_d.constant.width == 17U && one_d.constant.height == 1U &&
              one_d.constant.depth_or_layers == 1U,
          "1D texture dimensions decode as a width with unit height and depth");
    check(three_d && three_d.constant.width == 8U && three_d.constant.height == 10U &&
              three_d.constant.depth_or_layers == 3U,
          "3D texture dimensions decode their width, height and depth fields");
    check(cube && cube.constant.dimension == TextureDataDimension::cube &&
              cube.constant.width == 8192U && cube.constant.height == 8192U &&
              cube.constant.depth_or_layers == 64U,
          "cube texture dimensions decode without narrowing encoded maxima");
    auto not_texture = words;
    not_texture[0] = (not_texture[0] & ~0x3U) | 0x3U;
    check(!decode_texture_fetch_constant(not_texture),
          "texture fetch constant rejects non-texture fetch types");

    constexpr std::array<std::uint8_t, 2> requested_slots{1U, 31U};
    std::vector<std::uint32_t> registers(0x4800U + 32U * 6U);
    for (std::size_t i = 0U; i < words.size(); ++i) {
        registers[0x4800U + 1U * 6U + i] = words[i];
        registers[0x4800U + 31U * 6U + i] = words[i];
    }
    const auto bank = decode_texture_fetch_constant_bank(registers, requested_slots);
    check(bank && bank.constants[1].raw_words == words && bank.constants[31].raw_words == words,
          "texture fetch bank reads only requested six-dword slots through slot 31");
    constexpr std::array<std::uint8_t, 1> out_of_range_slot{32U};
    constexpr std::array<std::uint8_t, 1> truncated_slot{31U};
    const auto invalid_slot = decode_texture_fetch_constant_bank(registers, out_of_range_slot);
    const auto truncated = decode_texture_fetch_constant_bank(
        std::span<const std::uint32_t>(registers).first(registers.size() - 1U),
        truncated_slot);
    check(!invalid_slot && !truncated,
          "texture fetch bank rejects invalid slots and truncated register storage");

    const auto rgba8_layout = decode_texture_storage_layout(6U);
    const auto rgb565_layout = decode_texture_storage_layout(4U);
    const auto dxt1_layout = decode_texture_storage_layout(18U);
    const auto dxt5_layout = decode_texture_storage_layout(20U);
    const auto dxn_layout = decode_texture_storage_layout(49U);
    const auto ctx1_layout = decode_texture_storage_layout(60U);
    const auto rgba8_extended_layout = decode_texture_storage_layout(50U);
    const auto unsupported_layout = decode_texture_storage_layout(0U);
    check(rgba8_layout && rgba8_layout.layout.block_width_pixels == 1U &&
              rgba8_layout.layout.block_height_pixels == 1U &&
              rgba8_layout.layout.bytes_per_block == 4U && !rgba8_layout.layout.compressed,
          "RGBA8 texture storage is classified as four bytes per texel");
    check(rgb565_layout && rgb565_layout.layout.bytes_per_block == 2U &&
              !rgb565_layout.layout.compressed && rgba8_extended_layout &&
              rgba8_extended_layout.layout.bytes_per_block == 4U,
          "packed RGB565 and extended RGBA8 formats expose their source storage sizes");
    check(dxt1_layout && dxt1_layout.layout.block_width_pixels == 4U &&
              dxt1_layout.layout.block_height_pixels == 4U &&
              dxt1_layout.layout.bytes_per_block == 8U && dxt1_layout.layout.compressed,
          "DXT1 texture storage is classified as an eight-byte 4x4 block");
    check(dxt5_layout && dxt5_layout.layout.bytes_per_block == 16U &&
              dxt5_layout.layout.compressed && dxn_layout &&
              dxn_layout.layout.bytes_per_block == 16U && dxn_layout.layout.compressed &&
              ctx1_layout && ctx1_layout.layout.bytes_per_block == 8U &&
              ctx1_layout.layout.compressed,
          "DXT5, DXN and CTX1 texture storage use their encoded block sizes");
    check(!unsupported_layout,
          "unsupported Xenos texture storage formats fail explicitly");
}

void test_texture_block_rgba_decoding() {
    using namespace project360::gpu::xenos;

    constexpr std::array<std::byte, 4> rgba_storage{
        std::byte{0x33}, std::byte{0x22}, std::byte{0x11}, std::byte{0x44}};
    const auto rgba = decode_xenos_texture_block_rgba8(6U, rgba_storage);
    check(rgba && rgba.texel_count == 1U &&
              rgba.texels[0] == TextureRgba8{0x11U, 0x22U, 0x33U, 0x44U},
          "8:8:8:8 decoder extracts channels using Xenos format bit masks");

    constexpr std::array<std::byte, 2> rgb565_storage{
        std::byte{0x00}, std::byte{0xf8}};
    const auto rgb565 = decode_xenos_texture_block_rgba8(4U, rgb565_storage);
    check(rgb565 && rgb565.texel_count == 1U &&
              rgb565.texels[0] == TextureRgba8{255U, 0U, 0U, 255U},
          "5:6:5 decoder expands packed red, green and blue channels");

    constexpr std::array<std::byte, 8> dxt1_four_color{
        std::byte{0x00}, std::byte{0xf8}, std::byte{0xe0}, std::byte{0x07},
        std::byte{0xe4}, std::byte{0xe4}, std::byte{0xe4}, std::byte{0xe4}};
    const auto four_color = decode_xenos_texture_block_rgba8(18U, dxt1_four_color);
    check(four_color && four_color.texel_count == 16U &&
              four_color.texels[0] == TextureRgba8{255U, 0U, 0U, 255U} &&
              four_color.texels[1] == TextureRgba8{0U, 255U, 0U, 255U} &&
              four_color.texels[2] == TextureRgba8{170U, 85U, 0U, 255U} &&
              four_color.texels[3] == TextureRgba8{85U, 170U, 0U, 255U},
          "DXT1 decoder expands RGB565 endpoints and interpolates four-color palette");

    constexpr std::array<std::byte, 8> dxt1_transparent{
        std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
    const auto transparent = decode_xenos_texture_block_rgba8(18U, dxt1_transparent);
    check(transparent && transparent.texels[3] == TextureRgba8{0U, 0U, 0U, 0U},
          "DXT1 three-color mode decodes the transparent selector as transparent black");

    constexpr std::array<std::byte, 16> dxt3_explicit_alpha{
        std::byte{0x10}, std::byte{0x32}, std::byte{0x54}, std::byte{0x76},
        std::byte{0x98}, std::byte{0xba}, std::byte{0xdc}, std::byte{0xfe},
        std::byte{0x00}, std::byte{0xf8}, std::byte{0xe0}, std::byte{0x07},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    const auto dxt3 = decode_xenos_texture_block_rgba8(19U, dxt3_explicit_alpha);
    check(dxt3 && dxt3.texel_count == 16U &&
              dxt3.texels[0] == TextureRgba8{255U, 0U, 0U, 0U} &&
              dxt3.texels[1] == TextureRgba8{255U, 0U, 0U, 17U} &&
              dxt3.texels[15] == TextureRgba8{255U, 0U, 0U, 255U},
          "DXT3 decoder expands explicit 4-bit alpha and decodes its color block");

    constexpr std::array<std::byte, 16> dxt5_interpolated_alpha{
        std::byte{0xff}, std::byte{0x00}, std::byte{0xd0}, std::byte{0x01},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    const auto dxt5 = decode_xenos_texture_block_rgba8(20U, dxt5_interpolated_alpha);
    check(dxt5 && dxt5.texel_count == 16U && dxt5.texels[0].a == 255U &&
              dxt5.texels[1].a == 218U && dxt5.texels[2].a == 36U,
          "DXT5 decoder reads selectors for endpoint and interpolated alpha values");

    constexpr std::array<std::byte, 16> dxt5_alpha_extremes{
        std::byte{0x00}, std::byte{0xff}, std::byte{0x3e}, std::byte{0xff},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    const auto dxt5_extremes = decode_xenos_texture_block_rgba8(20U, dxt5_alpha_extremes);
    check(dxt5_extremes && dxt5_extremes.texels[0].a == 0U &&
              dxt5_extremes.texels[1].a == 255U,
          "DXT5 lower-endpoint mode produces its explicit transparent and opaque entries");

    constexpr std::array<std::byte, 7> truncated_dxt1{};
    const auto truncated = decode_xenos_texture_block_rgba8(18U, truncated_dxt1);
    const std::array<std::byte, 2> unsupported_storage{};
    const auto unsupported = decode_xenos_texture_block_rgba8(5U, unsupported_storage);
    const std::array<std::byte, 3> wrong_size{};
    const auto wrong_block_size = decode_xenos_texture_block_rgba8(6U, wrong_size);
    check(!truncated && !unsupported && !wrong_block_size,
          "texture block decoder rejects truncated, unsupported and mismatched storage inputs");
}

void test_tiled_texture_block_addressing_and_reads() {
    using namespace project360::gpu::xenos;
    const auto check_offset = [](std::uint32_t x, std::uint32_t y,
                                 std::uint32_t width, std::uint32_t height,
                                 std::uint32_t pitch, std::uint8_t bytes_per_block,
                                 std::uint64_t expected, const char* message) {
        const auto result = xenos_tiled_2d_block_offset(
            x, y, width, height, pitch, bytes_per_block);
        check(result && result.byte_offset == expected, message);
    };

    check_offset(0U, 0U, 64U, 64U, 64U, 4U, 0U,
                 "tiled 2D addressing starts the first block at the subresource base");
    check_offset(1U, 0U, 64U, 64U, 64U, 4U, 4U,
                 "tiled 2D addressing places adjacent X blocks contiguously");
    check_offset(0U, 1U, 64U, 64U, 64U, 4U, 16U,
                 "tiled 2D addressing interleaves the least significant Y bit");
    check_offset(0U, 2U, 64U, 64U, 64U, 4U, 256U,
                 "tiled 2D addressing maps inner microtile rows");
    check_offset(8U, 0U, 64U, 64U, 64U, 4U, 64U,
                 "tiled 2D addressing applies the X pipe selection");
    check_offset(0U, 8U, 64U, 64U, 64U, 4U, 1152U,
                 "tiled 2D addressing applies the Y pipe selection");
    check_offset(0U, 16U, 64U, 64U, 64U, 4U, 2048U,
                 "tiled 2D addressing applies bank selection");
    check_offset(32U, 0U, 64U, 64U, 64U, 4U, 4096U,
                 "tiled 2D addressing advances to the next macro tile in X");
    check_offset(0U, 32U, 64U, 64U, 64U, 4U, 8192U,
                 "tiled 2D addressing advances by aligned pitch between tile rows");

    for (const auto bytes_per_block : std::array<std::uint8_t, 5>{1U, 2U, 4U, 8U, 16U}) {
        const std::uint32_t period = bytes_per_block == 1U ? 128U
                                     : bytes_per_block == 2U ? 64U
                                                             : 32U;
        std::vector<bool> seen(static_cast<std::size_t>(period) * period, false);
        bool valid_permutation = true;
        for (std::uint32_t y = 0U; y < period; ++y) {
            for (std::uint32_t x = 0U; x < period; ++x) {
                const auto address = xenos_tiled_2d_block_offset(
                    x, y, period, period, period, bytes_per_block);
                if (!address || address.byte_offset % bytes_per_block != 0U) {
                    valid_permutation = false;
                    continue;
                }
                const auto index = static_cast<std::size_t>(
                    address.byte_offset / bytes_per_block);
                if (index >= seen.size() || seen[index]) {
                    valid_permutation = false;
                    continue;
                }
                seen[index] = true;
            }
        }
        check(valid_permutation &&
                  std::all_of(seen.begin(), seen.end(), [](bool present) { return present; }),
              "each Xenos format period maps bijectively with no block aliases");
    }

    check(!xenos_tiled_2d_block_offset(0U, 0U, 1U, 1U, 31U, 4U) &&
              !xenos_tiled_2d_block_offset(1U, 0U, 1U, 1U, 32U, 4U) &&
              !xenos_tiled_2d_block_offset(0U, 0U, 1U, 1U, 32U, 12U),
          "tiled texture addressing rejects bad pitch, out-of-range blocks and unsupported size");

    project360::memory::GuestMemory memory(0x5000U);
    constexpr std::uint64_t rgba_base = 0x100U;
    constexpr std::array<std::byte, 4> rgba_bytes{
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    check(memory.load(rgba_base + 4096U, rgba_bytes),
          "tiled RGBA fixture writes at its known macro-tile address");
    const auto rgba_block = read_xenos_tiled_2d_block(
        rgba_base, 32U, 0U, 64U, 64U, 64U, 4U, memory);
    check(rgba_block && rgba_block.physical_address == rgba_base + 4096U &&
              rgba_block.byte_count == rgba_bytes.size() &&
              std::equal(rgba_bytes.begin(), rgba_bytes.end(), rgba_block.bytes.begin()),
          "tiled texture read returns raw bytes from the computed physical block address");

    constexpr std::uint64_t dxt_base = 0x300U;
    constexpr std::array<std::byte, 8> dxt_bytes{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    check(memory.load(dxt_base + 16U, dxt_bytes),
          "tiled DXT fixture writes a full compressed storage block");
    const auto dxt_block = read_xenos_tiled_2d_block(
        dxt_base, 0U, 1U, 8U, 8U, 32U, 8U, memory);
    check(dxt_block && dxt_block.physical_address == dxt_base + 16U &&
              dxt_block.byte_count == dxt_bytes.size() &&
              std::equal(dxt_bytes.begin(), dxt_bytes.end(), dxt_block.bytes.begin()),
          "tiled texture read preserves all bytes of one compressed block");

    const auto out_of_memory = read_xenos_tiled_2d_block(
        0x4fffU, 0U, 0U, 1U, 1U, 32U, 4U, memory);
    const auto overflowing = read_xenos_tiled_2d_block(
        std::numeric_limits<std::uint64_t>::max() - 8U, 32U, 0U, 64U, 64U, 64U,
        4U, memory);
    check(!out_of_memory && !overflowing,
          "tiled texture block reads reject physical bounds and address overflow");

    const auto make_fetch = [](std::uint8_t format, std::uint32_t base,
                               std::uint32_t width, std::uint32_t height,
                               std::uint16_t pitch, bool tiled = true,
                               bool stacked = false, std::uint8_t endian = 0U) {
        std::array<std::uint32_t, 6> words{};
        words[0] = 2U | (static_cast<std::uint32_t>(pitch) << 22U) |
                   (tiled ? 0x80000000U : 0U);
        words[1] = format | (static_cast<std::uint32_t>(endian) << 6U) |
                   ((base >> 12U) << 12U) |
                   (stacked ? (1U << 10U) : 0U);
        words[2] = ((width - 1U) & 0x1fffU) |
                   (((height - 1U) & 0x1fffU) << 13U);
        words[5] = 1U << 9U;
        const auto decoded = decode_texture_fetch_constant(words);
        return decoded;
    };

    const auto rgba_fetch = make_fetch(6U, 0x1000U, 64U, 64U, 2U);
    constexpr std::array<std::byte, 4> descriptor_rgba_bytes{
        std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}, std::byte{0xdd}};
    check(rgba_fetch && memory.load(0x2000U, descriptor_rgba_bytes),
          "decoded RGBA texture fixture is valid and loads a known base-level block");
    if (rgba_fetch) {
        const auto descriptor_block = read_xenos_tiled_2d_texture_block(
            rgba_fetch.constant, 32U, 0U, memory);
        check(descriptor_block && descriptor_block.physical_address == 0x2000U &&
                  descriptor_block.byte_count == descriptor_rgba_bytes.size() &&
                  std::equal(descriptor_rgba_bytes.begin(), descriptor_rgba_bytes.end(),
                             descriptor_block.bytes.begin()),
              "decoded texture descriptor resolves pitch, dimensions and physical block address");
        if (descriptor_block) {
            const auto decoded_texel = decode_xenos_texture_block_rgba8(
                rgba_fetch.constant.format,
                std::span<const std::byte>(descriptor_block.bytes)
                    .first(descriptor_block.byte_count));
            check(decoded_texel && decoded_texel.texel_count == 1U &&
                      decoded_texel.texels[0] ==
                          TextureRgba8{0xccU, 0xbbU, 0xaaU, 0xddU},
                  "descriptor read and endian path feeds the RGBA8 block decoder correctly");
        }
        check(!read_xenos_tiled_2d_texture_block(rgba_fetch.constant, 64U, 0U, memory),
              "descriptor-backed texture read rejects coordinates outside visible dimensions");
    }

    constexpr std::array<std::byte, 4> endian_source{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    constexpr std::array<std::array<std::byte, 4>, 4> endian_expected{{
        {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}},
        {std::byte{0x22}, std::byte{0x11}, std::byte{0x44}, std::byte{0x33}},
        {std::byte{0x44}, std::byte{0x33}, std::byte{0x22}, std::byte{0x11}},
        {std::byte{0x33}, std::byte{0x44}, std::byte{0x11}, std::byte{0x22}},
    }};
    check(memory.load(0x1000U, endian_source),
          "texture endian fixture loads a known aligned physical dword");
    for (std::uint8_t endian = 0U; endian <= 3U; ++endian) {
        const auto endian_fetch = make_fetch(6U, 0x1000U, 64U, 64U, 2U, true, false,
                                             endian);
        if (!endian_fetch) {
            check(false, "endian fixture descriptor decodes");
            continue;
        }
        const auto endian_block = read_xenos_tiled_2d_texture_block(
            endian_fetch.constant, 0U, 0U, memory);
        check(endian_block &&
                  std::equal(endian_expected[endian].begin(), endian_expected[endian].end(),
                             endian_block.bytes.begin()),
              "descriptor reader applies the selected Xenos byte-lane endian mode");
    }

    constexpr std::array<std::byte, 4> byte_texels{
        std::byte{0xa1}, std::byte{0xb2}, std::byte{0xc3}, std::byte{0xd4}};
    const auto byte_endian_fetch = make_fetch(2U, 0x4000U, 64U, 64U, 2U, true, false, 2U);
    check(memory.load(0x4000U, byte_texels) && byte_endian_fetch,
          "single-byte format endian fixture is initialized");
    if (byte_endian_fetch) {
        const auto byte_endian_block = read_xenos_tiled_2d_texture_block(
            byte_endian_fetch.constant, 1U, 0U, memory);
        check(byte_endian_block && byte_endian_block.bytes[0] == std::byte{0xc3},
              "byte texel endian conversion reads the aligned dword lane group");
    }

    const auto dxt_fetch = make_fetch(18U, 0x3000U, 64U, 64U, 2U);
    constexpr std::array<std::byte, 8> descriptor_dxt_bytes{
        std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6},
        std::byte{5}, std::byte{4}, std::byte{3}, std::byte{2}};
    check(dxt_fetch && memory.load(0x3050U, descriptor_dxt_bytes),
          "decoded DXT1 fixture is valid and loads its compressed storage block");
    if (dxt_fetch) {
        const auto descriptor_block = read_xenos_tiled_2d_texture_block(
            dxt_fetch.constant, 32U, 4U, memory);
        check(descriptor_block && descriptor_block.physical_address == 0x3050U &&
                  descriptor_block.byte_count == descriptor_dxt_bytes.size() &&
                  std::equal(descriptor_dxt_bytes.begin(), descriptor_dxt_bytes.end(),
                             descriptor_block.bytes.begin()),
              "descriptor-backed DXT1 read converts texels into 4x4 storage blocks");
    }

    const auto stacked_fetch = make_fetch(6U, 0x1000U, 64U, 64U, 2U, true, true);
    const auto linear_fetch = make_fetch(6U, 0x1000U, 64U, 64U, 2U, false);
    const auto narrow_pitch_fetch = make_fetch(6U, 0x1000U, 64U, 64U, 1U);
    const auto unsupported_fetch = make_fetch(0U, 0x1000U, 64U, 64U, 2U);
    check(!stacked_fetch || !read_xenos_tiled_2d_texture_block(
                                stacked_fetch.constant, 0U, 0U, memory),
          "descriptor-backed reader reports stacked textures as unsupported");
    check(!linear_fetch || !read_xenos_tiled_2d_texture_block(
                               linear_fetch.constant, 0U, 0U, memory),
          "descriptor-backed reader reports linear texture layouts as unsupported");
    check(!narrow_pitch_fetch || !read_xenos_tiled_2d_texture_block(
                                     narrow_pitch_fetch.constant, 0U, 0U, memory),
          "descriptor-backed reader rejects a pitch narrower than visible texture width");
    check(!unsupported_fetch || !read_xenos_tiled_2d_texture_block(
                                    unsupported_fetch.constant, 0U, 0U, memory),
          "descriptor-backed reader propagates unsupported format diagnostics");
}

void test_vfetch_instruction_and_attribute_range() {
    using namespace project360::gpu::xenos;
    project360::memory::GuestMemory memory(0x1000U);
    const std::array<std::byte, 12> bytes{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
        std::byte{0x50}, std::byte{0x60}, std::byte{0x70}, std::byte{0x80},
        std::byte{0x90}, std::byte{0xa0}, std::byte{0xb0}, std::byte{0xc0}};
    check(memory.load(16U, bytes), "VFETCH attribute fixture loads into physical memory");
    const auto descriptor = decode_vertex_fetch_descriptor((4U << 2U) | 3U, 3U << 2U);
    const std::array<std::uint32_t, 3> words{
        (3U << 5U) | (1U << 11U) | (9U << 12U) | (1U << 18U) | (1U << 19U) |
            (2U << 20U) | (1U << 25U) | (2U << 27U) | (2U << 30U),
        0x00000688U | (37U << 16U) | (1U << 15U) | (1U << 14U) | (1U << 13U) |
            (1U << 12U) | (0x3fU << 24U),
        1U}; // One-dword stride and zero signed offset.
    const auto decoded = decode_vertex_fetch_instruction(words);
    check(static_cast<bool>(decoded) && decoded.instruction.fetch_constant_index == 7U &&
              decoded.instruction.component_count == 2U &&
              decoded.instruction.format == 37U && decoded.instruction.is_float_format &&
              decoded.instruction.stride_dwords == 1U &&
              decoded.instruction.offset_dwords == 0 &&
              decoded.instruction.exponent_adjust == -1 &&
              decoded.instruction.index_rounded && decoded.instruction.prefetch_count == 3U,
          "VFETCH decoder extracts constant selection, format, stride and signed fields");
    const auto selected_index = resolve_vertex_fetch_index(
        decoded.instruction, {1.0F, 2.0F, 30.25F, 4.0F});
    auto floor_instruction = decoded.instruction;
    floor_instruction.index_rounded = false;
    const auto floored_index = resolve_vertex_fetch_index(
        floor_instruction, {1.0F, 2.0F, 4.75F, 4.0F});
    const auto rounded_index = resolve_vertex_fetch_index(
        decoded.instruction, {1.0F, 2.0F, 4.5F, 4.0F});
    check(selected_index && selected_index.index == 30U && floored_index &&
              floored_index.index == 4U && rounded_index && rounded_index.index == 5U,
          "VFETCH selects the encoded source lane and applies floor or floor(x + 0.5)");
    const auto negative_index = resolve_vertex_fetch_index(
        floor_instruction, {1.0F, 2.0F, -0.25F, 4.0F});
    const auto infinite_index = resolve_vertex_fetch_index(
        floor_instruction, {1.0F, 2.0F, std::numeric_limits<float>::infinity(), 4.0F});
    check(!negative_index && !infinite_index,
          "VFETCH rejects negative and non-finite source indexes explicitly");
    auto encoded_negative_offset = words;
    encoded_negative_offset[2] = 0x7fffff01U;
    const auto decoded_negative_offset =
        decode_vertex_fetch_instruction(encoded_negative_offset);
    check(decoded_negative_offset && decoded_negative_offset.instruction.offset_dwords == -1,
          "VFETCH sign-extends the 23-bit signed dword offset");
    constexpr std::array<std::uint8_t, 15> formats{
        6U, 7U, 16U, 17U, 25U, 26U, 31U, 32U, 33U, 34U, 35U, 36U, 37U, 38U, 57U};
    constexpr std::array<std::uint8_t, 15> component_counts{
        4U, 4U, 3U, 3U, 2U, 4U, 2U, 4U, 1U, 2U, 4U, 1U, 2U, 4U, 3U};
    constexpr std::array<std::uint8_t, 15> storage_counts{
        1U, 1U, 1U, 1U, 1U, 2U, 1U, 2U, 1U, 2U, 4U, 1U, 2U, 4U, 3U};
    for (std::size_t index = 0U; index < formats.size(); ++index) {
        auto format_words = words;
        format_words[1] = (format_words[1] & ~(0x3fU << 16U)) |
                          (static_cast<std::uint32_t>(formats[index]) << 16U);
        const auto format = decode_vertex_fetch_instruction(format_words);
        check(format && format.instruction.component_count == component_counts[index] &&
                  format.instruction.storage_dword_count == storage_counts[index],
              "VFETCH distinguishes logical components from encoded dword storage");
    }
    if (decoded && descriptor) {
        const auto attribute =
            read_vertex_fetch_attribute(decoded.instruction, descriptor.descriptor, memory, 1U);
        check(static_cast<bool>(attribute) &&
                  attribute.words == std::vector<std::uint32_t>{0x80706050U, 0xc0b0a090U},
              "VFETCH uses the vertex index and dword stride to read vector components");

        auto offset_instruction = decoded.instruction;
        offset_instruction.offset_dwords = 1;
        const auto offset_attribute =
            read_vertex_fetch_attribute(offset_instruction, descriptor.descriptor, memory, 0U);
        check(static_cast<bool>(offset_attribute) &&
                  offset_attribute.words == std::vector<std::uint32_t>{0x80706050U,
                                                                        0xc0b0a090U},
              "VFETCH applies its signed dword offset before reading the attribute");

        auto negative_offset = decoded.instruction;
        negative_offset.offset_dwords = -1;
        const auto before_start =
            read_vertex_fetch_attribute(negative_offset, descriptor.descriptor, memory, 0U);
        check(!before_start && before_start.words.empty(),
              "VFETCH rejects a negative effective offset before the descriptor range");
        const auto past_end =
            read_vertex_fetch_attribute(decoded.instruction, descriptor.descriptor, memory, 2U);
        check(!past_end && past_end.words.empty(),
              "VFETCH rejects a multiword attribute that exceeds descriptor capacity");
    }

    auto mini_words = words;
    mini_words[1] |= 1U << 30U;
    auto predicated_words = words;
    predicated_words[1] |= 1U << 31U;
    auto reserved_words = words;
    reserved_words[1] |= 1U << 22U;
    auto invalid_selector_words = words;
    invalid_selector_words[0] = (invalid_selector_words[0] & ~(3U << 25U)) | (3U << 25U);
    auto wrong_opcode_words = words;
    wrong_opcode_words[0] |= 1U;
    auto missing_marker_words = words;
    missing_marker_words[0] &= ~(1U << 19U);
    check(!decode_vertex_fetch_instruction(mini_words) &&
              !decode_vertex_fetch_instruction(predicated_words) &&
              !decode_vertex_fetch_instruction(reserved_words) &&
              !decode_vertex_fetch_instruction(invalid_selector_words) &&
              !decode_vertex_fetch_instruction(wrong_opcode_words) &&
              !decode_vertex_fetch_instruction(missing_marker_words),
          "VFETCH unsupported, malformed and reserved encodings fail explicitly");

    auto packed_words = words;
    packed_words[1] = (packed_words[1] & ~(0x3fU << 16U)) | (6U << 16U);
    const auto packed = decode_vertex_fetch_instruction(packed_words);
    const auto packed_attribute = descriptor
                                      ? read_vertex_fetch_attribute(packed.instruction,
                                                                   descriptor.descriptor, memory,
                                                                   0U)
                                      : VertexFetchWordsResult{{}, "Invalid fixture"};
    check(packed && packed_attribute && packed_attribute.words.size() == 1U,
          "VFETCH recognizes packed formats and reads their actual storage width");

    const std::array<std::byte, 16> packed_data{
        std::byte{0x00}, std::byte{0x40}, std::byte{0x80}, std::byte{0xff}, // 8_8_8_8
        std::byte{0x00}, std::byte{0x02}, std::byte{0x01}, std::byte{0x00}, // 16_16
        std::byte{0x00}, std::byte{0x3c}, std::byte{0x00}, std::byte{0xc0}, // half 1, -2
        std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3f}}; // float 1
    check(memory.load(64U, packed_data), "VFETCH conversion fixtures load into guest memory");
    const auto packed_descriptor = decode_vertex_fetch_descriptor((16U << 2U) | 3U, 4U << 2U);
    auto make_format = [&](std::uint8_t format, bool integer, bool signed_components) {
        auto format_words = words;
        format_words[1] = (format_words[1] & ~((0x3fU << 24U) | (0x3fU << 16U) | (1U << 14U) |
                                               (1U << 13U) | (1U << 12U))) |
                          (static_cast<std::uint32_t>(format) << 16U) |
                          (integer ? 1U << 13U : 0U) |
                          (signed_components ? 1U << 12U : 0U);
        const auto result = decode_vertex_fetch_instruction(format_words);
        return result.instruction;
    };
    auto at_offset = [&](std::uint8_t format, bool integer, bool signed_components,
                         std::int32_t offset) {
        auto instruction = make_format(format, integer, signed_components);
        instruction.offset_dwords = offset;
        return instruction;
    };
    if (packed_descriptor) {
        const auto rgba = read_vertex_fetch_attribute_values(
            make_format(6U, false, false), packed_descriptor.descriptor, memory, 0U);
        check(rgba && rgba.components.size() == 4U && rgba.components[0] == 0.0F &&
                  rgba.components[1] == 64.0F / 255.0F &&
                  rgba.components[2] == 128.0F / 255.0F && rgba.components[3] == 1.0F,
              "VFETCH converts packed 8-bit unsigned normalized components");

        const std::array<std::byte, 4> snorm_bytes{
            std::byte{0x00}, std::byte{0xfe}, std::byte{0x07}, std::byte{0x80}};
        const auto signed_packed = memory.load(64U, snorm_bytes);
        const auto snorm = read_vertex_fetch_attribute_values(
            make_format(7U, false, true), packed_descriptor.descriptor, memory, 0U);
        check(signed_packed && snorm && snorm.components.size() == 4U &&
                  snorm.components[0] == -1.0F && snorm.components[1] == 1.0F &&
                  snorm.components[2] == 0.0F && snorm.components[3] == -1.0F,
              "VFETCH converts signed packed normalized components with minus-one clamping");

        auto unsupported_rf_instruction = make_format(7U, false, true);
        unsupported_rf_instruction.signed_repeating_fraction = true;
        check(!read_vertex_fetch_attribute_values(unsupported_rf_instruction,
                                                  packed_descriptor.descriptor, memory, 0U),
              "VFETCH rejects the unimplemented alternate signed-fraction mapping");

        const auto unsigned_int = read_vertex_fetch_attribute_values(
            at_offset(25U, true, false, 1), packed_descriptor.descriptor, memory, 0U);
        check(unsigned_int && unsigned_int.components == std::vector<float>{512.0F, 1.0F},
              "VFETCH converts packed 16-bit integer components without normalization");
        auto integer_exponent_adjust = at_offset(25U, true, false, 1);
        integer_exponent_adjust.exponent_adjust = 1;
        check(!read_vertex_fetch_attribute_values(integer_exponent_adjust,
                                                  packed_descriptor.descriptor, memory, 0U),
              "VFETCH reports unsupported exponent adjustment instead of ignoring it for integers");

        const auto half = read_vertex_fetch_attribute_values(
            at_offset(31U, false, false, 2), packed_descriptor.descriptor, memory, 0U);
        check(half && half.components == std::vector<float>{1.0F, -2.0F},
              "VFETCH converts two IEEE binary16 components to float");

        auto execution_instruction = at_offset(31U, false, false, 2);
        execution_instruction.destination_swizzle = static_cast<std::uint16_t>(
            0U | (1U << 3U) | (2U << 6U) | (3U << 9U));
        const auto execution = execute_vertex_fetch_attribute(
            execution_instruction, packed_descriptor.descriptor, memory,
            {0.0F, 0.0F, 0.0F, 0.0F}, {7.0F, 8.0F, 9.0F, 10.0F},
            {11.0F, 12.0F, 13.0F, 14.0F});
        check(execution && execution.vertex_index == 0U &&
                  execution.source_components == std::array<float, 4>{1.0F, -2.0F, 9.0F,
                                                                      10.0F} &&
                  execution.destination_components == execution.source_components,
              "VFETCH execution composes index, bounded conversion, explicit missing lanes and swizzle");
        const auto failed_execution = execute_vertex_fetch_attribute(
            execution_instruction, packed_descriptor.descriptor, memory,
            {0.0F, 0.0F, -1.0F, 0.0F}, {7.0F, 8.0F, 9.0F, 10.0F},
            {11.0F, 12.0F, 13.0F, 14.0F});
        check(!failed_execution && failed_execution.error.find("outside") != std::string::npos,
              "VFETCH execution propagates invalid source-index failures without a result");

        const std::array<std::byte, 8> batch_data{
            std::byte{0x00}, std::byte{0x3c}, std::byte{0x00}, std::byte{0xc0},
            std::byte{0x00}, std::byte{0x38}, std::byte{0x00}, std::byte{0x34}};
        const auto batch_loaded = memory.load(104U, batch_data);
        const auto batch_descriptor = decode_vertex_fetch_descriptor((24U << 2U) | 3U,
                                                                       4U << 2U);
        constexpr std::array<std::uint32_t, 2> batch_indices{0U, 1U};
        const auto batch = batch_loaded && batch_descriptor
                               ? execute_vertex_fetch_attribute_batch(
                                     execution_instruction, batch_descriptor.descriptor, memory,
                                     batch_indices, {99.0F, 98.0F, 97.0F, 96.0F},
                                     {7.0F, 8.0F, 9.0F, 10.0F},
                                     {11.0F, 12.0F, 13.0F, 14.0F})
                               : VertexFetchBatchResult{{}, "Invalid batch fixture"};
        check(batch && batch.destination_components.size() == 2U &&
                  batch.destination_components[0] == std::array<float, 4>{1.0F, -2.0F, 9.0F,
                                                                           10.0F} &&
                  batch.destination_components[1] == std::array<float, 4>{0.5F, 0.25F, 9.0F,
                                                                           10.0F},
              "VFETCH batch resolves each prepared vertex index through the encoded source lane");
        constexpr std::array<std::uint32_t, 2> invalid_batch_indices{0U, 4U};
        const auto failed_batch = batch_descriptor
                                      ? execute_vertex_fetch_attribute_batch(
                                            execution_instruction,
                                            batch_descriptor.descriptor, memory,
                                            invalid_batch_indices, {}, {7.0F, 8.0F, 9.0F, 10.0F},
                                            {11.0F, 12.0F, 13.0F, 14.0F})
                                      : VertexFetchBatchResult{{}, "Invalid batch fixture"};
        check(!failed_batch && failed_batch.destination_components.empty(),
              "VFETCH batch rejects out-of-range attributes without exposing partial output");

        std::array<VertexFetchDescriptor, 9> fetch_constants{};
        fetch_constants[7] = packed_descriptor.descriptor;
        if (batch_descriptor) fetch_constants[8] = batch_descriptor.descriptor;
        auto second_fetch_instruction = execution_instruction;
        second_fetch_instruction.fetch_constant_index = 8U;
        second_fetch_instruction.source_register = 4U;
        second_fetch_instruction.destination_register = 10U;
        second_fetch_instruction.source_register_relative = false;
        second_fetch_instruction.destination_register_relative = false;
        auto first_fetch_instruction = execution_instruction;
        first_fetch_instruction.source_register_relative = false;
        first_fetch_instruction.destination_register_relative = false;
        const std::array<VertexFetchInstruction, 2> fetch_program{
            first_fetch_instruction, second_fetch_instruction};
        VertexFetchRegisterBank initial_registers{};
        initial_registers[3][2] = 0.0F;
        initial_registers[4][2] = 1.0F;
        const auto fetch_execution = execute_vertex_fetch_instructions(
            fetch_program, fetch_constants, memory, initial_registers,
            {7.0F, 8.0F, 9.0F, 10.0F});
        check(fetch_execution &&
                  fetch_execution.registers[9] == std::array<float, 4>{1.0F, -2.0F, 9.0F,
                                                                        10.0F} &&
                  fetch_execution.registers[10] == std::array<float, 4>{0.5F, 0.25F, 9.0F,
                                                                         10.0F} &&
                  fetch_execution.registers[3] == initial_registers[3],
              "ordered VFETCH instructions resolve descriptor slots and write destination registers");
        auto missing_constant_program = fetch_program;
        missing_constant_program[1].fetch_constant_index = 9U;
        const auto failed_program = execute_vertex_fetch_instructions(
            missing_constant_program, fetch_constants, memory, initial_registers,
            {7.0F, 8.0F, 9.0F, 10.0F});
        check(!failed_program && failed_program.error.find("missing fetch-constant") !=
                                     std::string::npos &&
                  failed_program.registers[9] == std::array<float, 4>{},
              "VFETCH instruction list rejects a missing later descriptor without partial register state");

        const std::array<std::byte, 16> float_vector{
            std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0x3f},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xc0},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x3f},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0xbf}};
        const auto vector_loaded = memory.load(80U, float_vector);
        const auto vector_descriptor =
            decode_vertex_fetch_descriptor((20U << 2U) | 3U, 4U << 2U);
        const auto vector = vector_descriptor
                                ? read_vertex_fetch_attribute_values(
                                      make_format(38U, false, false),
                                      vector_descriptor.descriptor, memory, 0U)
                                : VertexFetchValuesResult{{}, "Invalid fixture"};
        check(vector_loaded && vector &&
                  vector.components == std::vector<float>{1.0F, -2.0F, 0.5F, -1.0F},
              "VFETCH extracts all four components from a 128-bit float attribute");

        auto swizzle_instruction = decoded.instruction;
        swizzle_instruction.destination_swizzle = static_cast<std::uint16_t>(
            0U | (5U << 3U) | (7U << 6U) | (4U << 9U));
        const auto swizzled = apply_vertex_fetch_destination_swizzle(
            swizzle_instruction, {10.0F, 20.0F, 30.0F, 40.0F},
            {1.0F, 2.0F, 3.0F, 4.0F});
        check(swizzled && swizzled.components == std::array<float, 4>{10.0F, 1.0F, 3.0F, 0.0F},
              "VFETCH destination swizzle selects lanes, constants and preserved values");
        swizzle_instruction.destination_swizzle = static_cast<std::uint16_t>(6U << 3U);
        check(!apply_vertex_fetch_destination_swizzle(
                  swizzle_instruction, {0.0F, 0.0F, 0.0F, 0.0F},
                  {0.0F, 0.0F, 0.0F, 0.0F}),
              "VFETCH rejects the reserved destination swizzle selector");
    }
}

void test_wait_for_idle_is_synchronous_barrier() {
    using namespace project360::gpu::xenos;
    std::vector<std::uint8_t> stream;
    append_u32_be(stream, 0xc0000000U | (0x26U << 8U)); // WAIT_FOR_IDLE, one dword.
    append_u32_be(stream, 0U);
    const auto decoded = decode_pm4(stream);
    std::vector<std::uint32_t> registers(1U, 0x12345678U);
    const auto applied = apply_register_packets(decoded.packets, stream, registers);
    check(static_cast<bool>(applied) && registers[0] == 0x12345678U,
          "WAIT_FOR_IDLE completes immediately with the synchronous unimplemented GPU");

    auto malformed = stream;
    malformed[1U] = 1U; // Two payload dwords instead of the required one.
    append_u32_be(malformed, 0U);
    const auto malformed_decoded = decode_pm4(malformed);
    const auto rejected = apply_register_packets(malformed_decoded.packets, malformed,
                                                 registers);
    check(!rejected && registers[0] == 0x12345678U,
          "malformed WAIT_FOR_IDLE packet is rejected without changing registers");
}

} // namespace

int main() {
    test_packet_framing_and_metadata();
    test_shader_alu_word_decode();
    test_shader_vector_alu_reference_subset();
    test_shader_vector_alu_comparisons();
    test_shader_vector_alu_rounding();
    test_shader_vector_alu_dot_products();
    test_shader_vector_alu_legacy_multiplication();
    test_shader_alu_temporary_source_resolution();
    test_shader_alu_absolute_float_constant_sources();
    test_shader_vector_alu_from_encoded_temporaries();
    test_shader_alu_add_mul_pair_execution();
    test_shader_exec_block_ordered_alu_and_vfetch();
    test_noops_and_malformed_streams();
    test_register_packet_application();
    test_type3_constant_uploads();
    test_unimplemented_commands_fail_explicitly();
    test_draw_packet_metadata_decoding();
    test_pm4_sequential_draw_dispatch();
    test_pm4_load_constant_banks_from_memory();
    test_xenos_scissor_register_resolution();
    test_xenos_viewport_register_resolution();
    test_dma_index_fetch();
    test_primitive_assembly();
    test_vertex_shader_index_transform();
    test_vertex_fetch_descriptors_and_memory();
    test_texture_fetch_constant_metadata();
    test_texture_block_rgba_decoding();
    test_tiled_texture_block_addressing_and_reads();
    test_vfetch_instruction_and_attribute_range();
    test_wait_for_idle_is_synchronous_barrier();
    if (failures != 0) {
        std::cerr << failures << " GPU packet test(s) failed\n";
        return 1;
    }
    std::cout << "GPU packet decoder tests passed\n";
    return 0;
}
