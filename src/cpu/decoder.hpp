#pragma once

#include <cstdint>
#include <string>

namespace project360::cpu {

enum class Opcode {
    addi,
    addis,
    mulli,
    ori,
    oris,
    xori,
    xoris,
    andi,
    andis,
    add,
    addc,
    adde,
    mullw,
    mulld,
    addic,
    and_register,
    and_complement_register,
    equivalent_register,
    nand_register,
    nor_register,
    or_register,
    or_complement_register,
    xor_register,
    extend_sign_byte,
    extend_sign_halfword,
    extend_sign_word,
    rotate_left_word_mask_insert,
    shift_left_word,
    shift_right_word,
    shift_right_algebraic_word,
    shift_right_algebraic_word_immediate,
    shift_left_doubleword,
    shift_right_doubleword,
    shift_right_algebraic_doubleword,
    rotate_left_word_mask,
    rotate_left_word_variable_mask,
    count_leading_zeros_word,
    count_leading_zeros_doubleword,
    subf,
    subfc,
    subfe,
    subfic,
    cmp,
    cmpl,
    cmpi,
    cmpli,
    lbz,
    lhz,
    lha,
    ld,
    stb,
    sth,
    lwz,
    std,
    stw,
    lmw,
    stmw,
    lhbrx,
    lwbrx,
    sthbrx,
    stwbrx,
    branch,
    branch_conditional,
    branch_lr,
    branch_ctr
};

struct DecodedInstruction {
    Opcode opcode{};
    std::uint8_t rt{};
    std::uint8_t ra{};
    std::uint8_t rb{};
    std::uint8_t bf{};
    std::uint8_t bo{};
    std::uint8_t bi{};
    std::uint8_t branch_hint{};
    std::uint8_t shift{};
    std::uint8_t mask_begin{};
    std::uint8_t mask_end{};
    std::int64_t immediate{};
    bool link{};
    bool absolute{};
    bool update{};
    bool indexed{};
    bool record{};
    bool wide{};
    bool overflow_enable{};
};

struct DecodeResult {
    DecodedInstruction instruction{};
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] DecodeResult decode(std::uint32_t word);
[[nodiscard]] std::uint32_t load_instruction_be(const std::uint8_t* bytes) noexcept;

} // namespace project360::cpu
