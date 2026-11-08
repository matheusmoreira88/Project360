#pragma once

#include <cstdint>
#include <string>

namespace project360::cpu {

enum class CrLogicalOperation {
    invalid,
    and_,
    and_complement,
    equivalent,
    nand,
    nor,
    or_,
    or_complement,
    xor_
};

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
    addme,
    addze,
    neg,
    mullw,
    mulld,
    mulhw,
    mulhwu,
    mulhd,
    mulhdu,
    divw,
    divwu,
    divd,
    divdu,
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
    shift_right_algebraic_doubleword_immediate,
    rotate_left_word_mask,
    rotate_left_word_variable_mask,
    rotate_left_doubleword_clear_left,
    rotate_left_doubleword_clear_right,
    rotate_left_doubleword_clear,
    rotate_left_doubleword_mask_insert,
    rotate_left_doubleword_variable_clear_left,
    rotate_left_doubleword_variable_clear_right,
    count_leading_zeros_word,
    count_leading_zeros_doubleword,
    population_count_bytes,
    subf,
    subfc,
    subfe,
    subfme,
    subfze,
    subfic,
    cmp,
    cmpl,
    cmpi,
    cmpli,
    trap_word_immediate,
    trap_doubleword_immediate,
    move_from_condition_register,
    move_condition_register_field,
    condition_register_logical,
    move_to_condition_register_fields,
    move_condition_register_from_xer,
    move_from_machine_state_register,
    move_to_machine_state_register_doubleword,
    move_from_special_register,
    move_to_special_register,
    lbz,
    lhz,
    lha,
    lfs,
    lfd,
    load_word_and_reserve_indexed,
    load_doubleword_and_reserve_indexed,
    ld,
    stb,
    sth,
    lwz,
    stfs,
    stfd,
    stfiwx,
    store_word_conditional_indexed,
    store_doubleword_conditional_indexed,
    data_cache_block_store,
    data_cache_block_flush,
    data_cache_block_touch,
    data_cache_block_touch_store,
    data_cache_block_set_to_zero,
    instruction_cache_block_invalidate,
    std,
    stw,
    lmw,
    stmw,
    load_string_word_immediate,
    load_string_word_indexed,
    store_string_word_immediate,
    store_string_word_indexed,
    lhbrx,
    lwbrx,
    sthbrx,
    stwbrx,
    branch,
    branch_conditional,
    branch_lr,
    branch_ctr,
    isync,
    sync,
    lwsync,
    return_from_interrupt_doubleword,
    system_call,
    floating_move_register,
    floating_move_from_fpscr,
    floating_move_condition_register_from_fpscr,
    floating_move_to_fpscr_field_immediate,
    floating_move_to_fpscr_bit_zero,
    floating_move_to_fpscr_bit_one,
    floating_negate,
    floating_absolute,
    floating_negative_absolute,
    floating_select,
    floating_compare_unordered,
    floating_compare_ordered,
    trap_word,
    trap_doubleword
};

struct DecodedInstruction {
    Opcode opcode{};
    std::uint8_t rt{};
    std::uint8_t ra{};
    std::uint8_t rb{};
    std::uint8_t frc{};
    std::uint8_t bf{};
    std::uint8_t bfa{};
    CrLogicalOperation cr_logical_operation{};
    std::uint16_t spr{};
    std::uint8_t bo{};
    std::uint8_t bi{};
    std::uint8_t to{};
    std::uint8_t bt{};
    std::uint8_t level{};
    std::uint8_t branch_hint{};
    std::uint8_t shift{};
    std::uint8_t mask_begin{};
    std::uint8_t mask_end{};
    std::uint8_t field_mask{};
    std::uint8_t byte_count{};
    std::int64_t immediate{};
    bool link{};
    bool absolute{};
    bool update{};
    bool indexed{};
    bool record{};
    bool msr_l{};
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
