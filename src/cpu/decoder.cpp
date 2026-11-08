#include "cpu/decoder.hpp"

#include <bit>

namespace project360::cpu {
namespace {

bool valid_branch_options(std::uint8_t bo) {
    if (bo <= 0x0fU) return true;
    if (bo == 0x10U || bo == 0x11U || bo == 0x12U || bo == 0x13U || bo == 0x14U ||
        bo == 0x18U || bo == 0x19U || bo == 0x1aU || bo == 0x1bU) {
        return true;
    }
    return false;
}

bool register_in_sequence(std::uint8_t candidate, std::uint8_t first,
                         std::uint8_t register_count) {
    for (std::uint8_t index = 0U; index < register_count; ++index) {
        if (static_cast<std::uint8_t>((first + index) & 0x1fU) == candidate) return true;
    }
    return false;
}

} // namespace

DecodeResult decode(std::uint32_t word) {
    const auto primary = static_cast<std::uint8_t>(word >> 26U);
    const auto rt = static_cast<std::uint8_t>((word >> 21U) & 0x1fU);
    const auto ra = static_cast<std::uint8_t>((word >> 16U) & 0x1fU);
    const auto rb = static_cast<std::uint8_t>((word >> 11U) & 0x1fU);
    const auto imm16 = static_cast<std::uint16_t>(word & 0xffffU);
    DecodedInstruction ins{};
    ins.rt = rt;
    ins.ra = ra;
    ins.rb = rb;

    switch (primary) {
    case 2:
    case 3:
        ins.to = rt;
        ins.opcode = primary == 2U ? Opcode::trap_doubleword_immediate
                                   : Opcode::trap_word_immediate;
        ins.immediate = std::bit_cast<std::int16_t>(imm16);
        return {ins, {}};
    case 14: ins.opcode = Opcode::addi; ins.immediate = std::bit_cast<std::int16_t>(imm16); return {ins, {}};
    case 15: ins.opcode = Opcode::addis; ins.immediate = std::bit_cast<std::int16_t>(imm16); return {ins, {}};
    case 7: ins.opcode = Opcode::mulli; ins.immediate = std::bit_cast<std::int16_t>(imm16); return {ins, {}};
    case 8: ins.opcode = Opcode::subfic; ins.immediate = std::bit_cast<std::int16_t>(imm16); return {ins, {}};
    case 12:
    case 13:
        ins.opcode = Opcode::addic;
        ins.record = primary == 13U;
        ins.immediate = std::bit_cast<std::int16_t>(imm16);
        return {ins, {}};
    case 21:
        ins.opcode = Opcode::rotate_left_word_mask;
        ins.shift = static_cast<std::uint8_t>((word >> 11U) & 0x1fU);
        ins.mask_begin = static_cast<std::uint8_t>((word >> 6U) & 0x1fU);
        ins.mask_end = static_cast<std::uint8_t>((word >> 1U) & 0x1fU);
        ins.record = (word & 1U) != 0U;
        return {ins, {}};
    case 20:
    case 23:
        ins.opcode = primary == 20U ? Opcode::rotate_left_word_mask_insert
                                    : Opcode::rotate_left_word_variable_mask;
        ins.shift = static_cast<std::uint8_t>((word >> 11U) & 0x1fU);
        ins.mask_begin = static_cast<std::uint8_t>((word >> 6U) & 0x1fU);
        ins.mask_end = static_cast<std::uint8_t>((word >> 1U) & 0x1fU);
        ins.record = (word & 1U) != 0U;
        return {ins, {}};
    case 30: {
        const auto mds_xo = static_cast<std::uint8_t>((word >> 1U) & 0xfU);
        if (mds_xo == 8U || mds_xo == 9U) {
            ins.opcode = mds_xo == 8U ? Opcode::rotate_left_doubleword_variable_clear_left
                                      : Opcode::rotate_left_doubleword_variable_clear_right;
            ins.mask_begin = static_cast<std::uint8_t>((word >> 5U) & 0x3fU);
            ins.mask_end = ins.mask_begin;
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }

        const auto md_xo = static_cast<std::uint8_t>((word >> 2U) & 0x7U);
        switch (md_xo) {
        case 0: ins.opcode = Opcode::rotate_left_doubleword_clear_left; break;
        case 1: ins.opcode = Opcode::rotate_left_doubleword_clear_right; break;
        case 2: ins.opcode = Opcode::rotate_left_doubleword_clear; break;
        case 3: ins.opcode = Opcode::rotate_left_doubleword_mask_insert; break;
        default: return {{}, "reserved doubleword rotate-mask subopcode"};
        }
        ins.shift = static_cast<std::uint8_t>(((word >> 11U) & 0x1fU) |
                                              ((word & 0x2U) << 4U));
        ins.mask_begin = static_cast<std::uint8_t>((word >> 5U) & 0x3fU);
        ins.mask_end = ins.mask_begin;
        ins.record = (word & 1U) != 0U;
        return {ins, {}};
    }
    case 10:
    case 11:
        if ((word & (1U << 22U)) != 0U) return {{}, "compare immediate reserved bit is set"};
        ins.opcode = primary == 10U ? Opcode::cmpli : Opcode::cmpi;
        ins.bf = static_cast<std::uint8_t>((word >> 23U) & 0x7U);
        ins.wide = (word & (1U << 21U)) != 0U;
        ins.ra = static_cast<std::uint8_t>((word >> 16U) & 0x1fU);
        ins.immediate = primary == 10U ? static_cast<std::int64_t>(imm16)
                                       : static_cast<std::int64_t>(std::bit_cast<std::int16_t>(imm16));
        return {ins, {}};
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
        ins.opcode = primary == 24U ? Opcode::ori : primary == 25U ? Opcode::oris
                     : primary == 26U ? Opcode::xori : primary == 27U ? Opcode::xoris
                     : primary == 28U ? Opcode::andi : Opcode::andis;
        ins.ra = rt;
        ins.rt = ra;
        ins.immediate = imm16;
        ins.record = primary == 28U || primary == 29U;
        return {ins, {}};
    case 32: case 33: case 34: case 35: case 36: case 37: case 38:
    case 39: case 40: case 41: case 42: case 43: case 44: case 45: {
        switch (primary) {
        case 32: case 33: ins.opcode = Opcode::lwz; ins.update = primary == 33U; break;
        case 34: case 35: ins.opcode = Opcode::lbz; ins.update = primary == 35U; break;
        case 36: case 37: ins.opcode = Opcode::stw; ins.update = primary == 37U; break;
        case 38: case 39: ins.opcode = Opcode::stb; ins.update = primary == 39U; break;
        case 40: case 41: ins.opcode = Opcode::lhz; ins.update = primary == 41U; break;
        case 42: case 43: ins.opcode = Opcode::lha; ins.update = primary == 43U; break;
        case 44: case 45: ins.opcode = Opcode::sth; ins.update = primary == 45U; break;
        default: break;
        }
        ins.immediate = std::bit_cast<std::int16_t>(imm16);
        if (ins.update && ins.ra == 0U) return {{}, "update load/store requires RA != 0"};
        const bool is_load = ins.opcode == Opcode::lbz || ins.opcode == Opcode::lhz ||
                             ins.opcode == Opcode::lha || ins.opcode == Opcode::lwz;
        if (ins.update && is_load && ins.rt == ins.ra) {
            return {{}, "update load requires RT != RA"};
        }
        return {ins, {}};
    }
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: {
        switch (primary) {
        case 48: case 49: ins.opcode = Opcode::lfs; ins.update = primary == 49U; break;
        case 50: case 51: ins.opcode = Opcode::lfd; ins.update = primary == 51U; break;
        case 52: case 53: ins.opcode = Opcode::stfs; ins.update = primary == 53U; break;
        case 54: case 55: ins.opcode = Opcode::stfd; ins.update = primary == 55U; break;
        default: break;
        }
        ins.immediate = std::bit_cast<std::int16_t>(imm16);
        if (ins.update && ins.ra == 0U) {
            return {{}, "floating-point update load/store requires RA != 0"};
        }
        return {ins, {}};
    }
    case 58:
    case 62: {
        const auto ds_xo = word & 0x3U;
        if (primary == 58U) {
            if (ds_xo > 1U) return {{}, "unsupported or reserved load DS-form subopcode"};
            ins.opcode = Opcode::ld;
            ins.update = ds_xo == 1U;
            if (ins.update && (ins.ra == 0U || ins.rt == ins.ra)) {
                return {{}, "ldu requires RA != 0 and RT != RA"};
            }
        } else {
            if (ds_xo > 1U) return {{}, "reserved store DS-form subopcode"};
            ins.opcode = Opcode::std;
            ins.update = ds_xo == 1U;
            if (ins.update && ins.ra == 0U) return {{}, "stdu requires RA != 0"};
        }
        const auto ds = static_cast<std::uint16_t>((word >> 2U) & 0x3fffU);
        ins.immediate = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(ds << 2U));
        return {ins, {}};
    }
    case 63: {
        const auto xo10 = static_cast<std::uint16_t>((word >> 1U) & 0x3ffU);
        const auto xo5 = static_cast<std::uint8_t>((word >> 1U) & 0x1fU);
        if (xo5 == 23U) {
            ins.opcode = Opcode::floating_select;
            ins.rt = static_cast<std::uint8_t>((word >> 6U) & 0x1fU);
            ins.ra = rb;
            ins.rb = ra;
            ins.frc = rt;
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }
        if (xo10 == 0U || xo10 == 32U) {
            if ((ins.rt & 0x3U) != 0U || (word & 1U) != 0U) {
                return {{}, "floating-point compare has reserved fields set"};
            }
            ins.bf = static_cast<std::uint8_t>((word >> 23U) & 0x7U);
            ins.opcode = xo10 == 0U ? Opcode::floating_compare_unordered
                                    : Opcode::floating_compare_ordered;
            return {ins, {}};
        }
        switch (xo10) {
        case 72: ins.opcode = Opcode::floating_move_register; break;
        case 40: ins.opcode = Opcode::floating_negate; break;
        case 264: ins.opcode = Opcode::floating_absolute; break;
        case 136: ins.opcode = Opcode::floating_negative_absolute; break;
        default:
            return {{}, "unsupported floating-point extended opcode " +
                             std::to_string(xo10)};
        }
        if (ins.ra != 0U) {
            return {{}, "floating-point unary instruction reserved FRA field is nonzero"};
        }
        ins.record = (word & 1U) != 0U;
        return {ins, {}};
    }
    case 46:
    case 47:
        ins.opcode = primary == 46U ? Opcode::lmw : Opcode::stmw;
        ins.immediate = std::bit_cast<std::int16_t>(imm16);
        if (ins.opcode == Opcode::lmw &&
            ((ins.ra == 0U && ins.rt == 0U) || (ins.ra != 0U && ins.ra >= ins.rt))) {
            return {{}, "lmw RA overlaps its destination register range"};
        }
        return {ins, {}};
    case 18: {
        const auto li = word & 0x03fffffcU;
        ins.opcode = Opcode::branch;
        ins.immediate = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(li << 6U) >> 6U);
        ins.link = (word & 1U) != 0U;
        ins.absolute = (word & 2U) != 0U;
        return {ins, {}};
    }
    case 16: {
        ins.opcode = Opcode::branch_conditional;
        ins.bo = static_cast<std::uint8_t>((word >> 21U) & 0x1fU);
        ins.bi = static_cast<std::uint8_t>((word >> 16U) & 0x1fU);
        ins.immediate = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(word & 0xfffcU));
        ins.link = (word & 1U) != 0U;
        ins.absolute = (word & 2U) != 0U;
        if (!valid_branch_options(ins.bo)) return {{}, "reserved branch option encoding"};
        return {ins, {}};
    }
    case 17:
        if (word != ((17U << 26U) | 2U) && word != ((17U << 26U) | (1U << 5U) | 2U)) {
            return {{}, "system-call instruction has reserved fields set"};
        }
        ins.opcode = Opcode::system_call;
        ins.level = (word & (1U << 5U)) != 0U ? 1U : 0U;
        return {ins, {}};
    case 19: {
        const auto xo10 = static_cast<std::uint16_t>((word >> 1U) & 0x3ffU);
        if (xo10 == 150U) {
            if (ins.rt != 0U || ins.ra != 0U || ins.rb != 0U || (word & 1U) != 0U) {
                return {{}, "isync reserved fields are nonzero"};
            }
            ins.opcode = Opcode::isync;
            return {ins, {}};
        }
        if (xo10 == 18U) {
            constexpr std::uint32_t reserved_mask = (0x7fffU << 6U) | 1U;
            if ((word & reserved_mask) != 0U) {
                return {{}, "rfid reserved fields are nonzero"};
            }
            ins.opcode = Opcode::return_from_interrupt_doubleword;
            return {ins, {}};
        }
        switch (xo10) {
        case 257U: ins.cr_logical_operation = CrLogicalOperation::and_; break;
        case 129U: ins.cr_logical_operation = CrLogicalOperation::and_complement; break;
        case 289U: ins.cr_logical_operation = CrLogicalOperation::equivalent; break;
        case 225U: ins.cr_logical_operation = CrLogicalOperation::nand; break;
        case 33U: ins.cr_logical_operation = CrLogicalOperation::nor; break;
        case 449U: ins.cr_logical_operation = CrLogicalOperation::or_; break;
        case 417U: ins.cr_logical_operation = CrLogicalOperation::or_complement; break;
        case 193U: ins.cr_logical_operation = CrLogicalOperation::xor_; break;
        default: break;
        }
        if (ins.cr_logical_operation != CrLogicalOperation::invalid) {
            if ((word & 1U) != 0U) {
                return {{}, "condition-register logical Rc bit is reserved"};
            }
            ins.opcode = Opcode::condition_register_logical;
            return {ins, {}};
        }
        if (xo10 == 0U) {
            constexpr std::uint32_t reserved_mask = (0x3U << 21U) | (0x3U << 16U) |
                                                    (0x1fU << 11U) | 1U;
            if ((word & reserved_mask) != 0U) {
                return {{}, "mcrf reserved fields are nonzero"};
            }
            ins.opcode = Opcode::move_condition_register_field;
            ins.bf = static_cast<std::uint8_t>((word >> 23U) & 0x7U);
            ins.bfa = static_cast<std::uint8_t>((word >> 18U) & 0x7U);
            return {ins, {}};
        }
        if (xo10 != 16U && xo10 != 528U) {
            return {{}, "unsupported branch-register extended opcode " + std::to_string(xo10)};
        }
        ins.opcode = xo10 == 16U ? Opcode::branch_lr : Opcode::branch_ctr;
        ins.bo = rt;
        ins.bi = ra;
        ins.branch_hint = static_cast<std::uint8_t>((word >> 11U) & 0x3U);
        ins.link = (word & 1U) != 0U;
        if ((word & 0xe000U) != 0U) return {{}, "reserved branch-register field is nonzero"};
        if (!valid_branch_options(ins.bo)) return {{}, "reserved branch option encoding"};
        if (ins.branch_hint == 1U || ins.branch_hint == 2U) {
            return {{}, "reserved branch hint encoding"};
        }
        if (ins.opcode == Opcode::branch_ctr && (ins.bo & 0x04U) == 0U) {
            return {{}, "bcctr must not decrement CTR"};
        }
        return {ins, {}};
    }
    case 31: {
        const auto xo = static_cast<std::uint16_t>((word >> 1U) & 0x1ffU);
        const auto xo10 = static_cast<std::uint16_t>((word >> 1U) & 0x3ffU);
        const bool oe = (word & (1U << 10U)) != 0U;
        if (xo == 4U || xo == 68U) {
            if ((word & ((1U << 10U) | 1U)) != 0U) {
                return {{}, "trap instruction reserved XO or Rc field is nonzero"};
            }
            ins.opcode = xo == 4U ? Opcode::trap_word : Opcode::trap_doubleword;
            ins.to = rt;
            return {ins, {}};
        }
        if (xo10 == 83U) {
            if ((word & (0x3ffU << 11U)) != 0U || (word & 1U) != 0U) {
                return {{}, "mfmsr reserved fields are nonzero"};
            }
            ins.opcode = Opcode::move_from_machine_state_register;
            return {ins, {}};
        }
        if (xo10 == 178U) {
            constexpr std::uint32_t reserved_fields = (0x1fU << 11U) | (0xfU << 17U) | 1U;
            if ((word & reserved_fields) != 0U) {
                return {{}, "mtmsrd reserved fields are nonzero"};
            }
            ins.opcode = Opcode::move_to_machine_state_register_doubleword;
            ins.msr_l = ((word >> 16U) & 1U) != 0U;
            return {ins, {}};
        }
        if (xo10 == 339U || xo10 == 467U) {
            if ((word & 1U) != 0U) {
                return {{}, "mfspr/mtspr record bit is reserved"};
            }
            ins.spr = static_cast<std::uint16_t>((ins.rb << 5U) | ins.ra);
            if (ins.spr != 1U && ins.spr != 8U && ins.spr != 9U && ins.spr != 26U &&
                ins.spr != 27U) {
                return {{}, "unsupported special-purpose register " + std::to_string(ins.spr)};
            }
            ins.opcode = xo10 == 339U ? Opcode::move_from_special_register
                                      : Opcode::move_to_special_register;
            return {ins, {}};
        }
        if (xo10 == 19U) {
            if (ins.ra != 0U || ins.rb != 0U || (word & 1U) != 0U) {
                return {{}, "mfcr reserved fields are nonzero"};
            }
            ins.opcode = Opcode::move_from_condition_register;
            return {ins, {}};
        }
        if (xo10 == 144U) {
            if ((word & ((1U << 20U) | (1U << 11U) | 1U)) != 0U) {
                return {{}, "mtcrf reserved fields are nonzero"};
            }
            ins.opcode = Opcode::move_to_condition_register_fields;
            ins.field_mask = static_cast<std::uint8_t>((word >> 12U) & 0xffU);
            return {ins, {}};
        }
        if (xo10 == 512U) {
            if (((word >> 21U) & 0x3U) != 0U || ins.ra != 0U || ins.rb != 0U ||
                (word & 1U) != 0U) {
                return {{}, "mcrxr reserved fields are nonzero"};
            }
            ins.opcode = Opcode::move_condition_register_from_xer;
            ins.bf = static_cast<std::uint8_t>((word >> 23U) & 0x7U);
            return {ins, {}};
        }
        if (xo10 == 20U) {
            if ((word & 1U) != 0U) return {{}, "lwarx reserved record bit is set"};
            ins.opcode = Opcode::load_word_and_reserve_indexed;
            ins.indexed = true;
            return {ins, {}};
        }
        if (xo10 == 84U) {
            if ((word & 1U) != 0U) return {{}, "ldarx reserved record bit is set"};
            ins.opcode = Opcode::load_doubleword_and_reserve_indexed;
            ins.indexed = true;
            return {ins, {}};
        }
        if (xo10 == 598U) {
            if ((ins.rt > 1U) || ins.ra != 0U || ins.rb != 0U || (word & 1U) != 0U) {
                return {{}, "sync reserved fields are nonzero or L is reserved"};
            }
            ins.opcode = ins.rt == 0U ? Opcode::sync : Opcode::lwsync;
            return {ins, {}};
        }
        if (xo10 == 150U) {
            if ((word & 1U) == 0U) return {{}, "stwcx. requires its record bit"};
            ins.opcode = Opcode::store_word_conditional_indexed;
            ins.indexed = true;
            ins.record = true;
            return {ins, {}};
        }
        if (xo10 == 214U) {
            if ((word & 1U) == 0U) return {{}, "stdcx. requires its record bit"};
            ins.opcode = Opcode::store_doubleword_conditional_indexed;
            ins.indexed = true;
            ins.record = true;
            return {ins, {}};
        }
        if (xo10 == 597U || xo10 == 725U) {
            if ((word & 1U) != 0U) return {{}, "string instruction record bit is reserved"};
            const auto count = ins.rb == 0U ? 32U : static_cast<unsigned>(ins.rb);
            const auto register_count = static_cast<std::uint8_t>((count + 3U) / 4U);
            if (xo10 == 597U &&
                (register_in_sequence(ins.ra, ins.rt, register_count) ||
                 (ins.rt == 0U && ins.ra == 0U))) {
                return {{}, "lswi base register overlaps its destination register range"};
            }
            ins.opcode = xo10 == 597U ? Opcode::load_string_word_immediate
                                      : Opcode::store_string_word_immediate;
            ins.byte_count = static_cast<std::uint8_t>(count);
            return {ins, {}};
        }
        if (xo10 == 533U || xo10 == 661U) {
            if ((word & 1U) != 0U) return {{}, "string instruction record bit is reserved"};
            if (xo10 == 533U && (ins.rt == ins.ra || ins.rt == ins.rb)) {
                return {{}, "lswx target register aliases an address register"};
            }
            ins.opcode = xo10 == 533U ? Opcode::load_string_word_indexed
                                      : Opcode::store_string_word_indexed;
            ins.indexed = true;
            return {ins, {}};
        }
        if (xo10 == 54U || xo10 == 86U || xo10 == 982U) {
            if (ins.rt != 0U || (word & 1U) != 0U) {
                return {{}, "cache-maintenance instruction has reserved fields set"};
            }
            ins.opcode = xo10 == 54U ? Opcode::data_cache_block_store
                         : xo10 == 86U ? Opcode::data_cache_block_flush
                                       : Opcode::instruction_cache_block_invalidate;
            return {ins, {}};
        }
        if (xo10 == 246U || xo10 == 278U) {
            if ((word & 1U) != 0U) {
                return {{}, "cache-touch instruction record bit is reserved"};
            }
            const auto touch_hint = ins.rt;
            if (touch_hint != 0U && touch_hint != 1U && touch_hint != 3U &&
                touch_hint != 8U && touch_hint != 10U) {
                return {{}, "unsupported data-cache touch hint " +
                                 std::to_string(touch_hint)};
            }
            ins.opcode = xo10 == 246U ? Opcode::data_cache_block_touch_store
                                      : Opcode::data_cache_block_touch;
            return {ins, {}};
        }
        if (xo10 == 1014U) {
            if (ins.rt != 0U || (word & 1U) != 0U) {
                return {{}, "dcbz reserved fields are nonzero"};
            }
            ins.opcode = Opcode::data_cache_block_set_to_zero;
            return {ins, {}};
        }
        if (xo10 == 21U || xo10 == 53U || xo10 == 149U || xo10 == 181U ||
            xo10 == 23U || xo10 == 55U || xo10 == 87U || xo10 == 119U ||
            xo10 == 279U || xo10 == 331U || xo10 == 343U || xo10 == 375U ||
            xo10 == 151U || xo10 == 183U || xo10 == 215U || xo10 == 247U ||
            xo10 == 535U || xo10 == 567U || xo10 == 599U || xo10 == 631U ||
            xo10 == 663U || xo10 == 695U || xo10 == 727U || xo10 == 759U ||
            xo10 == 407U || xo10 == 439U || xo10 == 534U || xo10 == 662U ||
            xo10 == 790U || xo10 == 918U || xo10 == 983U) {
            if ((word & 1U) != 0U) return {{}, "indexed load/store record bit is reserved"};
            switch (xo10) {
            case 21: case 53: ins.opcode = Opcode::ld; break;
            case 23: case 55: ins.opcode = Opcode::lwz; break;
            case 87: case 119: ins.opcode = Opcode::lbz; break;
            case 279: case 331: ins.opcode = Opcode::lhz; break;
            case 343: case 375: ins.opcode = Opcode::lha; break;
            case 151: case 183: ins.opcode = Opcode::stw; break;
            case 149: case 181: ins.opcode = Opcode::std; break;
            case 215: case 247: ins.opcode = Opcode::stb; break;
            case 407: case 439: ins.opcode = Opcode::sth; break;
            case 534: ins.opcode = Opcode::lwbrx; break;
            case 662: ins.opcode = Opcode::stwbrx; break;
            case 790: ins.opcode = Opcode::lhbrx; break;
            case 918: ins.opcode = Opcode::sthbrx; break;
            case 535: case 567: ins.opcode = Opcode::lfs; break;
            case 599: case 631: ins.opcode = Opcode::lfd; break;
            case 663: case 695: ins.opcode = Opcode::stfs; break;
            case 727: case 759: ins.opcode = Opcode::stfd; break;
            case 983: ins.opcode = Opcode::stfiwx; break;
            default: break;
            }
            ins.indexed = true;
            ins.update = xo10 == 53U || xo10 == 55U || xo10 == 119U || xo10 == 331U ||
                         xo10 == 375U || xo10 == 181U || xo10 == 183U || xo10 == 247U ||
                         xo10 == 439U || xo10 == 567U || xo10 == 631U || xo10 == 695U ||
                         xo10 == 759U;
            if (ins.update && ins.ra == 0U) return {{}, "indexed update requires RA != 0"};
            const bool is_load = ins.opcode == Opcode::ld || ins.opcode == Opcode::lbz ||
                                 ins.opcode == Opcode::lhz || ins.opcode == Opcode::lha ||
                                 ins.opcode == Opcode::lwz || ins.opcode == Opcode::lfs ||
                                 ins.opcode == Opcode::lfd;
            if (ins.update && is_load && ins.opcode != Opcode::lfs &&
                ins.opcode != Opcode::lfd && ins.rt == ins.ra) {
                return {{}, "indexed update load requires RT != RA"};
            }
            return {ins, {}};
        }
        if (xo10 == 28U || xo10 == 60U || xo10 == 284U || xo10 == 476U ||
            xo10 == 124U || xo10 == 444U || xo10 == 412U || xo10 == 316U) {
            switch (xo10) {
            case 28: ins.opcode = Opcode::and_register; break;
            case 60: ins.opcode = Opcode::and_complement_register; break;
            case 284: ins.opcode = Opcode::equivalent_register; break;
            case 476: ins.opcode = Opcode::nand_register; break;
            case 124: ins.opcode = Opcode::nor_register; break;
            case 444: ins.opcode = Opcode::or_register; break;
            case 412: ins.opcode = Opcode::or_complement_register; break;
            case 316: ins.opcode = Opcode::xor_register; break;
            default: break;
            }
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }
        if (xo10 == 922U || xo10 == 954U || xo10 == 986U) {
            if (ins.rb != 0U) return {{}, "sign-extension instruction reserved field is nonzero"};
            ins.opcode = xo10 == 922U ? Opcode::extend_sign_halfword
                         : xo10 == 954U ? Opcode::extend_sign_byte
                                         : Opcode::extend_sign_word;
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }
        if (xo10 == 24U || xo10 == 536U || xo10 == 792U) {
            ins.opcode = xo10 == 24U ? Opcode::shift_left_word
                         : xo10 == 536U ? Opcode::shift_right_word
                                        : Opcode::shift_right_algebraic_word;
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }
        if (xo10 == 824U) {
            ins.opcode = Opcode::shift_right_algebraic_word_immediate;
            ins.shift = ins.rb;
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }
        if (xo10 == 826U || xo10 == 827U) {
            ins.opcode = Opcode::shift_right_algebraic_doubleword_immediate;
            ins.shift = static_cast<std::uint8_t>(((word >> 11U) & 0x1fU) |
                                                  ((word & 0x2U) << 4U));
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }
        if (xo10 == 27U || xo10 == 539U || xo10 == 794U) {
            ins.opcode = xo10 == 27U ? Opcode::shift_left_doubleword
                         : xo10 == 539U ? Opcode::shift_right_doubleword
                                        : Opcode::shift_right_algebraic_doubleword;
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }
        if (xo10 == 26U || xo10 == 58U) {
            if (ins.rb != 0U) return {{}, "count-leading-zeros reserved field is nonzero"};
            ins.opcode = xo10 == 26U ? Opcode::count_leading_zeros_word
                                     : Opcode::count_leading_zeros_doubleword;
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }
        if (xo10 == 122U) {
            if (ins.rb != 0U || (word & 1U) != 0U) {
                return {{}, "popcntb reserved fields are nonzero"};
            }
            ins.opcode = Opcode::population_count_bytes;
            return {ins, {}};
        }
        if (xo == 0U || xo == 32U) {
            if ((word & (1U << 22U)) != 0U) return {{}, "compare register reserved bit is set"};
            if ((word & 1U) != 0U) return {{}, "compare register record bit is reserved"};
            ins.opcode = xo == 0U ? Opcode::cmp : Opcode::cmpl;
            ins.bf = static_cast<std::uint8_t>((word >> 23U) & 0x7U);
            ins.wide = (word & (1U << 21U)) != 0U;
            ins.ra = static_cast<std::uint8_t>((word >> 16U) & 0x1fU);
            ins.rb = static_cast<std::uint8_t>((word >> 11U) & 0x1fU);
            return {ins, {}};
        }
        if (xo == 266U || xo == 10U || xo == 138U) {
            ins.opcode = xo == 266U ? Opcode::add : (xo == 10U ? Opcode::addc : Opcode::adde);
            ins.record = (word & 1U) != 0U;
            ins.overflow_enable = oe;
            return {ins, {}};
        }
        if (xo == 202U) {
            if (ins.rb != 0U) return {{}, "addze reserved RB field is nonzero"};
            ins.opcode = Opcode::addze;
            ins.record = (word & 1U) != 0U;
            ins.overflow_enable = oe;
            return {ins, {}};
        }
        if (xo == 234U) {
            if (ins.rb != 0U) return {{}, "addme reserved RB field is nonzero"};
            ins.opcode = Opcode::addme;
            ins.record = (word & 1U) != 0U;
            ins.overflow_enable = oe;
            return {ins, {}};
        }
        if (xo == 200U) {
            if (ins.rb != 0U) return {{}, "subfze reserved RB field is nonzero"};
            ins.opcode = Opcode::subfze;
            ins.record = (word & 1U) != 0U;
            ins.overflow_enable = oe;
            return {ins, {}};
        }
        if (xo == 232U) {
            if (ins.rb != 0U) return {{}, "subfme reserved RB field is nonzero"};
            ins.opcode = Opcode::subfme;
            ins.record = (word & 1U) != 0U;
            ins.overflow_enable = oe;
            return {ins, {}};
        }
        if (xo == 235U || xo == 233U) {
            ins.opcode = xo == 235U ? Opcode::mullw : Opcode::mulld;
            ins.record = (word & 1U) != 0U;
            ins.overflow_enable = oe;
            return {ins, {}};
        }
        if (xo == 75U || xo == 11U) {
            ins.opcode = xo == 75U ? Opcode::mulhw : Opcode::mulhwu;
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }
        if (xo == 73U || xo == 9U) {
            ins.opcode = xo == 73U ? Opcode::mulhd : Opcode::mulhdu;
            ins.record = (word & 1U) != 0U;
            return {ins, {}};
        }
        if (xo == 491U || xo == 459U || xo == 489U || xo == 457U) {
            ins.opcode = xo == 491U ? Opcode::divw
                         : xo == 459U ? Opcode::divwu
                         : xo == 489U ? Opcode::divd
                                      : Opcode::divdu;
            ins.record = (word & 1U) != 0U;
            ins.overflow_enable = oe;
            return {ins, {}};
        }
        if (xo == 40U || xo == 8U || xo == 136U) {
            ins.opcode = xo == 40U ? Opcode::subf : (xo == 8U ? Opcode::subfc : Opcode::subfe);
            ins.record = (word & 1U) != 0U;
            ins.overflow_enable = oe;
            return {ins, {}};
        }
        if (xo == 104U) {
            if (ins.rb != 0U) return {{}, "neg reserved RB field is nonzero"};
            ins.opcode = Opcode::neg;
            ins.record = (word & 1U) != 0U;
            ins.overflow_enable = oe;
            return {ins, {}};
        }
        return {{}, "unsupported extended opcode " + std::to_string(xo)};
    }
    default: return {{}, "unsupported primary opcode " + std::to_string(primary)};
    }
}

std::uint32_t load_instruction_be(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

} // namespace project360::cpu
