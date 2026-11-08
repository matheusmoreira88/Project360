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
    case 19: {
        const auto xo10 = static_cast<std::uint16_t>((word >> 1U) & 0x3ffU);
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
        if (xo10 == 21U || xo10 == 53U || xo10 == 149U || xo10 == 181U ||
            xo10 == 23U || xo10 == 55U || xo10 == 87U || xo10 == 119U ||
            xo10 == 279U || xo10 == 331U || xo10 == 343U || xo10 == 375U ||
            xo10 == 151U || xo10 == 183U || xo10 == 215U || xo10 == 247U ||
            xo10 == 407U || xo10 == 439U || xo10 == 534U || xo10 == 662U ||
            xo10 == 790U || xo10 == 918U) {
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
            default: break;
            }
            ins.indexed = true;
            ins.update = xo10 == 53U || xo10 == 55U || xo10 == 119U || xo10 == 331U ||
                         xo10 == 375U || xo10 == 181U || xo10 == 183U || xo10 == 247U ||
                         xo10 == 439U;
            if (ins.update && ins.ra == 0U) return {{}, "indexed update requires RA != 0"};
            const bool is_load = ins.opcode == Opcode::ld || ins.opcode == Opcode::lbz ||
                                 ins.opcode == Opcode::lhz || ins.opcode == Opcode::lha ||
                                 ins.opcode == Opcode::lwz;
            if (ins.update && is_load && ins.rt == ins.ra) {
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
        if (xo == 235U || xo == 233U) {
            ins.opcode = xo == 235U ? Opcode::mullw : Opcode::mulld;
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
