#include "cpu/interpreter.hpp"

#include "cpu/decoder.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace project360::cpu {
namespace {

constexpr std::uint32_t kXerSummaryOverflow = 0x80000000U;
constexpr std::uint32_t kXerOverflow = 0x40000000U;
constexpr std::uint32_t kXerCarry = 0x20000000U;
constexpr std::uint32_t kXerByteCountMask = 0x7fU;
constexpr std::uint64_t kProgramExceptionVector = 0x700U;
constexpr std::uint64_t kXenonDataCacheBlockSize = 128U;

constexpr std::uint64_t powerpc_bit(unsigned bit) {
    return std::uint64_t{1} << (63U - bit);
}

std::uint64_t saved_msr_for_srr1(std::uint64_t msr) {
    std::uint64_t saved = 0;
    for (unsigned bit = 0; bit < 64U; ++bit) {
        if (bit <= 32U || (bit >= 37U && bit <= 41U) || bit >= 48U) {
            saved |= msr & powerpc_bit(bit);
        }
    }
    return saved;
}

std::uint64_t program_exception_srr1(std::uint64_t msr, unsigned cause_bit) {
    return saved_msr_for_srr1(msr) | powerpc_bit(cause_bit);
}

StepResult take_system_call_exception(CpuState& state) {
    constexpr std::uint64_t kSystemCallExceptionVector = 0xc00U;
    const auto fault_pc = state.pc;
    state.srr0 = fault_pc + 4U;
    state.srr1 = saved_msr_for_srr1(state.msr);

    // Apply the modeled OEA interrupt-entry state. SF, HV and ME are retained;
    // Xenon hypervisor routing and LPCR/LPES-dependent state are not modeled.
    state.msr = powerpc_bit(0U) | (state.msr & powerpc_bit(3U)) |
                (state.msr & powerpc_bit(51U));
    state.pc = kSystemCallExceptionVector;
    return {StepStatus::exception_taken, "PowerPC system call exception (LEV=0)",
            std::nullopt, GuestException{GuestExceptionKind::system_call,
                                         kSystemCallExceptionVector}};
}

StepResult take_program_exception(CpuState& state, FaultKind cause, unsigned syndrome_bit,
                                  std::uint32_t instruction, std::string message) {
    const auto fault_pc = state.pc;
    state.srr0 = fault_pc;
    state.srr1 = program_exception_srr1(state.msr, syndrome_bit);

    // The base OEA entry clears translation, interrupt, floating-point, trace,
    // privilege, and recoverable-interrupt state; ME and current HV are retained.
    state.msr = powerpc_bit(0U) | (state.msr & powerpc_bit(3U)) |
                (state.msr & powerpc_bit(51U));
    state.pc = kProgramExceptionVector;

    return {StepStatus::exception_taken, std::move(message),
            ExecutionFault{cause, fault_pc, instruction, std::nullopt},
            GuestException{GuestExceptionKind::program, kProgramExceptionVector}};
}

std::uint64_t restore_msr_from_srr1(std::uint64_t old_msr, std::uint64_t srr1) {
    std::uint64_t restored = 0;
    const auto copy_range = [&](unsigned first, unsigned last) {
        for (unsigned bit = first; bit <= last; ++bit) {
            restored |= srr1 & powerpc_bit(bit);
        }
    };
    copy_range(1U, 2U);
    copy_range(4U, 32U);
    copy_range(37U, 41U);
    copy_range(49U, 50U);
    copy_range(52U, 57U);
    copy_range(60U, 63U);

    const auto get_bit = [](std::uint64_t value, unsigned bit) {
        return (value & powerpc_bit(bit)) != 0U;
    };
    const auto set_bit = [&](unsigned bit, bool value) {
        if (value) restored |= powerpc_bit(bit);
    };
    set_bit(0U, get_bit(srr1, 0U) || get_bit(srr1, 1U));
    set_bit(3U, get_bit(old_msr, 3U) && get_bit(srr1, 3U));
    set_bit(48U, get_bit(srr1, 48U) || get_bit(srr1, 49U));
    set_bit(51U, (get_bit(old_msr, 3U) && get_bit(srr1, 51U)) ||
                     (!get_bit(old_msr, 3U) && get_bit(old_msr, 51U)));
    set_bit(58U, get_bit(srr1, 58U) || get_bit(srr1, 49U));
    set_bit(59U, get_bit(srr1, 59U) || get_bit(srr1, 49U));
    return restored;
}

StepResult fault(FaultKind kind, std::uint64_t pc, std::string message,
                 std::optional<std::uint32_t> instruction = std::nullopt,
                 std::optional<std::uint64_t> effective_address = std::nullopt) {
    return {StepStatus::fault, std::move(message),
            ExecutionFault{kind, pc, instruction, effective_address}, std::nullopt};
}

struct AddResult {
    std::uint64_t value{};
    bool carry{};
    bool overflow{};
};

AddResult add_values(std::uint64_t left, std::uint64_t right, bool carry_in) {
    const auto partial = left + right;
    const bool carry_from_partial = partial < left;
    const auto result = partial + static_cast<std::uint64_t>(carry_in);
    const bool carry_from_input = result < partial;
    const auto lower_sum = (left & 0x7fffffffffffffffULL) +
                           (right & 0x7fffffffffffffffULL) + static_cast<std::uint64_t>(carry_in);
    const bool carry_into_sign = (lower_sum >> 63U) != 0U;
    const bool carry_out = carry_from_partial || carry_from_input;
    const bool overflow = carry_into_sign != carry_out;
    return {result, carry_from_partial || carry_from_input, overflow};
}

void set_xer_overflow(CpuState& state, bool overflow) {
    state.xer = (state.xer & ~kXerOverflow) | (overflow ? kXerOverflow : 0U);
    if (overflow) state.xer |= kXerSummaryOverflow;
}

void set_xer_carry(CpuState& state, bool carry) {
    state.xer = (state.xer & ~kXerCarry) | (carry ? kXerCarry : 0U);
}

bool signed_multiply_overflow(std::int64_t left, std::int64_t right) {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (left == 0 || right == 0) return false;
    if (left == -1) return right == minimum;
    if (right == -1) return left == minimum;
    if (left > 0) {
        return right > 0 ? left > maximum / right : right < minimum / left;
    }
    return right > 0 ? left < minimum / right : right < maximum / left;
}

std::uint64_t multiply_high_unsigned(std::uint64_t left, std::uint64_t right) {
    constexpr std::uint64_t kLowWordMask = 0xffffffffULL;
    const auto left_low = left & kLowWordMask;
    const auto left_high = left >> 32U;
    const auto right_low = right & kLowWordMask;
    const auto right_high = right >> 32U;

    const auto low_product = left_low * right_low;
    const auto low_cross_product = left_low * right_high;
    const auto high_cross_product = left_high * right_low;
    const auto high_product = left_high * right_high;
    const auto middle = (low_product >> 32U) + (low_cross_product & kLowWordMask) +
                        (high_cross_product & kLowWordMask);
    return high_product + (low_cross_product >> 32U) + (high_cross_product >> 32U) +
           (middle >> 32U);
}

std::uint64_t multiply_high_signed(std::uint64_t left, std::uint64_t right) {
    auto high = multiply_high_unsigned(left, right);
    if ((left & 0x8000000000000000ULL) != 0U) high -= right;
    if ((right & 0x8000000000000000ULL) != 0U) high -= left;
    return high;
}

void set_cr_field(CpuState& state, std::uint8_t bf, std::uint32_t field) {
    const auto shift = static_cast<unsigned>(28U - 4U * bf);
    const auto mask = 0xfU << shift;
    state.cr = (state.cr & ~mask) | ((field & 0xfU) << shift);
}

std::uint32_t compare_field(bool less, bool greater, bool summary_overflow) {
    std::uint32_t field = 0;
    if (less) field = 0x8U;
    else if (greater) field = 0x4U;
    else field = 0x2U;
    if (summary_overflow) field |= 1U;
    return field;
}

std::uint32_t rotate_mask(std::uint8_t begin, std::uint8_t end) {
    std::uint32_t mask = 0;
    for (unsigned bit = 0; bit < 32U; ++bit) {
        const bool selected = begin <= end ? (bit >= begin && bit <= end)
                                          : (bit >= begin || bit <= end);
        if (selected) mask |= 1U << (31U - bit);
    }
    return mask;
}

std::uint64_t rotate_mask_doubleword(std::uint8_t begin, std::uint8_t end) {
    std::uint64_t mask = 0;
    for (unsigned bit = 0; bit < 64U; ++bit) {
        const bool selected = begin <= end ? (bit >= begin && bit <= end)
                                          : (bit >= begin || bit <= end);
        if (selected) mask |= 1ULL << (63U - bit);
    }
    return mask;
}

bool evaluate_branch_condition(CpuState& state, std::uint8_t bo, std::uint8_t bi) {
    const bool ignore_condition = (bo & 0x10U) != 0U;
    const bool expected_condition = (bo & 0x08U) != 0U;
    const bool condition_bit = ((state.cr >> (31U - bi)) & 1U) != 0U;
    const bool condition_ok = ignore_condition || condition_bit == expected_condition;

    const bool ignore_count = (bo & 0x04U) != 0U;
    const bool expect_zero_count = (bo & 0x02U) != 0U;
    if (!ignore_count) --state.ctr;
    const bool count_ok = ignore_count || ((state.ctr != 0U) != expect_zero_count);
    return condition_ok && count_ok;
}

void set_cr0(CpuState& state, std::uint64_t value) {
    const auto signed_value = std::bit_cast<std::int64_t>(value);
    set_cr_field(state, 0, compare_field(signed_value < 0, signed_value > 0,
                                         (state.xer & 0x80000000U) != 0U));
}

bool register_in_sequence(std::uint8_t candidate, std::uint8_t first,
                          std::size_t register_count) {
    for (std::size_t index = 0U; index < register_count; ++index) {
        if (static_cast<std::uint8_t>((first + index) & 0x1fU) == candidate) return true;
    }
    return false;
}

} // namespace

StepResult Interpreter::step(CpuState& state, memory::GuestMemory& memory) const {
    if ((state.pc & 3U) != 0U) {
        return fault(FaultKind::instruction_alignment, state.pc,
                     "unaligned instruction address");
    }
    std::uint32_t word = 0;
    if (!memory.fetch_u32_be(state.pc, word)) {
        return fault(FaultKind::instruction_access, state.pc, "instruction fetch out of range");
    }
    const auto decoded = decode(word);
    if (!decoded) {
        return take_program_exception(state, FaultKind::illegal_instruction, 44U, word,
                                       "PC 0x" + std::to_string(state.pc) + ": " +
                                           decoded.error);
    }

    const auto ins = decoded.instruction;
    const auto old_pc = state.pc;
    auto next_pc = old_pc + 4U;
    const auto base = ins.ra == 0 ? 0U : state.gpr[ins.ra];
    const auto data_fault = [&](std::uint64_t address, std::size_t width,
                                std::string message) {
        const auto kind = width > 1U && address % width != 0U ? FaultKind::data_alignment
                                                               : FaultKind::data_access;
        return fault(kind, old_pc, std::move(message), word, address);
    };
    const auto internal_fault = [&](std::string message) {
        return fault(FaultKind::interpreter_internal, old_pc, std::move(message), word);
    };
    switch (ins.opcode) {
    case Opcode::system_call:
        if (ins.level == 1U) {
            return fault(FaultKind::unsupported_instruction, old_pc,
                         "sc LEV=1 hypervisor call is unsupported", word);
        }
        return take_system_call_exception(state);
    case Opcode::trap_word_immediate:
    case Opcode::trap_doubleword_immediate:
    case Opcode::trap_word:
    case Opcode::trap_doubleword: {
        const bool word_operation = ins.opcode == Opcode::trap_word_immediate ||
                                    ins.opcode == Opcode::trap_word;
        const bool immediate = ins.opcode == Opcode::trap_word_immediate ||
                               ins.opcode == Opcode::trap_doubleword_immediate;
        const auto left_bits = word_operation
                                   ? static_cast<std::uint64_t>(
                                         static_cast<std::uint32_t>(state.gpr[ins.ra]))
                                   : state.gpr[ins.ra];
        const auto right_bits = immediate
                                    ? (word_operation
                                           ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(
                                                 ins.immediate))
                                           : static_cast<std::uint64_t>(ins.immediate))
                                    : (word_operation
                                           ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(
                                                 state.gpr[ins.rb]))
                                           : state.gpr[ins.rb]);
        const bool less_signed = word_operation
                                     ? std::bit_cast<std::int32_t>(
                                           static_cast<std::uint32_t>(left_bits)) <
                                           std::bit_cast<std::int32_t>(
                                               static_cast<std::uint32_t>(right_bits))
                                     : std::bit_cast<std::int64_t>(left_bits) <
                                           std::bit_cast<std::int64_t>(right_bits);
        const bool greater_signed = word_operation
                                        ? std::bit_cast<std::int32_t>(
                                              static_cast<std::uint32_t>(left_bits)) >
                                              std::bit_cast<std::int32_t>(
                                                  static_cast<std::uint32_t>(right_bits))
                                        : std::bit_cast<std::int64_t>(left_bits) >
                                              std::bit_cast<std::int64_t>(right_bits);
        const bool equal = left_bits == right_bits;
        const bool less_unsigned = left_bits < right_bits;
        const bool greater_unsigned = left_bits > right_bits;
        const auto to = ins.to;
        const bool trap = ((to & 0x10U) != 0U && less_signed) ||
                          ((to & 0x08U) != 0U && greater_signed) ||
                          ((to & 0x04U) != 0U && equal) ||
                          ((to & 0x02U) != 0U && less_unsigned) ||
                          ((to & 0x01U) != 0U && greater_unsigned);
        if (trap) {
            return take_program_exception(state, FaultKind::trap, 46U, word,
                                          "PowerPC integer trap condition matched");
        }
        break;
    }
    case Opcode::isync:
        // Guest instructions are fetched from memory on every step, so this
        // interpreter has no prefetched host instruction cache to invalidate.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        break;
    case Opcode::sync:
    case Opcode::lwsync:
        // GuestMemory completes and synchronizes each bounded access before
        // returning. The full host fence prevents native/compiler reordering
        // across this instruction; Xenon-specific weak ordering is not modeled.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        break;
    case Opcode::data_cache_block_store:
    case Opcode::data_cache_block_flush:
    case Opcode::data_cache_block_touch:
    case Opcode::data_cache_block_touch_store:
    case Opcode::instruction_cache_block_invalidate:
        // GuestMemory is coherent and has no CPU or translated-code cache.
        // Cache maintenance and prefetch hints therefore have no state to change.
        break;
    case Opcode::data_cache_block_set_to_zero: {
        const auto effective_address = base + state.gpr[ins.rb];
        const auto block_address = effective_address & ~(kXenonDataCacheBlockSize - 1U);
        if (!memory.fill(block_address, static_cast<std::size_t>(kXenonDataCacheBlockSize),
                         std::byte{0})) {
            return fault(FaultKind::data_access, old_pc,
                         "dcbz target cache block is not writable guest memory", word,
                         block_address);
        }
        break;
    }
    case Opcode::floating_move_register:
    case Opcode::floating_negate:
    case Opcode::floating_absolute:
    case Opcode::floating_negative_absolute: {
        constexpr std::uint64_t kSignBit = 0x8000000000000000ULL;
        const auto source = state.fpr[ins.rb];
        switch (ins.opcode) {
        case Opcode::floating_move_register: state.fpr[ins.rt] = source; break;
        case Opcode::floating_negate: state.fpr[ins.rt] = source ^ kSignBit; break;
        case Opcode::floating_absolute: state.fpr[ins.rt] = source & ~kSignBit; break;
        case Opcode::floating_negative_absolute: state.fpr[ins.rt] = source | kSignBit; break;
        default: return internal_fault("invalid floating-point unary opcode");
        }
        if (ins.record) set_cr_field(state, 1U, (state.fpscr >> 28U) & 0xfU);
        break;
    }
    case Opcode::floating_compare_unordered:
    case Opcode::floating_compare_ordered: {
        constexpr std::uint64_t kExponentMask = 0x7ff0000000000000ULL;
        constexpr std::uint64_t kFractionMask = 0x000fffffffffffffULL;
        constexpr std::uint64_t kQuietBit = 0x0008000000000000ULL;
        constexpr std::uint32_t kFpscrFpccMask = 0x0000f000U;
        const auto left_bits = state.fpr[ins.ra];
        const auto right_bits = state.fpr[ins.rb];
        const auto is_nan = [&](std::uint64_t bits) {
            return (bits & kExponentMask) == kExponentMask && (bits & kFractionMask) != 0U;
        };
        const auto is_signaling_nan = [&](std::uint64_t bits) {
            return is_nan(bits) && (bits & kQuietBit) == 0U;
        };
        const bool left_nan = is_nan(left_bits);
        const bool right_nan = is_nan(right_bits);
        if (left_nan || right_nan) {
            if (ins.opcode == Opcode::floating_compare_ordered ||
                is_signaling_nan(left_bits) || is_signaling_nan(right_bits)) {
                return fault(FaultKind::unsupported_instruction, old_pc,
                             "floating compare NaN exception behavior is unsupported", word);
            }
            constexpr std::uint32_t kUnordered = 0x1U;
            set_cr_field(state, ins.bf, kUnordered);
            state.fpscr = (state.fpscr & ~kFpscrFpccMask) | (kUnordered << 12U);
            break;
        }

        const auto left = std::bit_cast<double>(left_bits);
        const auto right = std::bit_cast<double>(right_bits);
        const std::uint32_t fpcc = left < right ? 0x8U : (left > right ? 0x4U : 0x2U);
        set_cr_field(state, ins.bf, fpcc);
        state.fpscr = (state.fpscr & ~kFpscrFpccMask) | (fpcc << 12U);
        break;
    }
    case Opcode::addi:
        state.gpr[ins.rt] = base + static_cast<std::uint64_t>(ins.immediate);
        break;
    case Opcode::mulli: {
        const auto multiplier = static_cast<std::uint64_t>(ins.immediate);
        state.gpr[ins.rt] = state.gpr[ins.ra] * multiplier;
        break;
    }
    case Opcode::addis:
        state.gpr[ins.rt] = base + (static_cast<std::uint64_t>(ins.immediate) << 16U);
        break;
    case Opcode::ori:
        state.gpr[ins.rt] = state.gpr[ins.ra] | static_cast<std::uint64_t>(ins.immediate);
        break;
    case Opcode::oris:
        state.gpr[ins.rt] = state.gpr[ins.ra] | (static_cast<std::uint64_t>(ins.immediate) << 16U);
        break;
    case Opcode::xori:
        state.gpr[ins.rt] = state.gpr[ins.ra] ^ static_cast<std::uint64_t>(ins.immediate);
        break;
    case Opcode::xoris:
        state.gpr[ins.rt] = state.gpr[ins.ra] ^ (static_cast<std::uint64_t>(ins.immediate) << 16U);
        break;
    case Opcode::andi:
    case Opcode::andis: {
        const auto immediate = static_cast<std::uint64_t>(ins.immediate) <<
                               (ins.opcode == Opcode::andis ? 16U : 0U);
        const auto value = state.gpr[ins.ra] & immediate;
        state.gpr[ins.rt] = value;
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::and_register:
    case Opcode::and_complement_register:
    case Opcode::equivalent_register:
    case Opcode::nand_register:
    case Opcode::nor_register:
    case Opcode::or_register:
    case Opcode::or_complement_register:
    case Opcode::xor_register: {
        const auto left = state.gpr[ins.rt];
        const auto right = state.gpr[ins.rb];
        std::uint64_t value = 0;
        switch (ins.opcode) {
        case Opcode::and_register: value = left & right; break;
        case Opcode::and_complement_register: value = left & ~right; break;
        case Opcode::equivalent_register: value = ~(left ^ right); break;
        case Opcode::nand_register: value = ~(left & right); break;
        case Opcode::nor_register: value = ~(left | right); break;
        case Opcode::or_register: value = left | right; break;
        case Opcode::or_complement_register: value = left | ~right; break;
        case Opcode::xor_register: value = left ^ right; break;
        default: break;
        }
        state.gpr[ins.ra] = value;
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::extend_sign_byte:
    case Opcode::extend_sign_halfword:
    case Opcode::extend_sign_word: {
        std::int64_t value = 0;
        if (ins.opcode == Opcode::extend_sign_byte) {
            const auto low = static_cast<std::uint8_t>(state.gpr[ins.rt]);
            value = std::bit_cast<std::int8_t>(low);
        } else if (ins.opcode == Opcode::extend_sign_halfword) {
            const auto low = static_cast<std::uint16_t>(state.gpr[ins.rt]);
            value = std::bit_cast<std::int16_t>(low);
        } else {
            const auto low = static_cast<std::uint32_t>(state.gpr[ins.rt]);
            value = std::bit_cast<std::int32_t>(low);
        }
        state.gpr[ins.ra] = static_cast<std::uint64_t>(value);
        if (ins.record) set_cr0(state, state.gpr[ins.ra]);
        break;
    }
    case Opcode::rotate_left_word_mask:
    case Opcode::rotate_left_word_mask_insert:
    case Opcode::rotate_left_word_variable_mask: {
        const auto source = static_cast<std::uint32_t>(state.gpr[ins.rt]);
        const auto shift = ins.opcode == Opcode::rotate_left_word_variable_mask
                               ? static_cast<int>(state.gpr[ins.rb] & 0x1fU)
                               : static_cast<int>(ins.shift);
        const auto rotated = std::rotl(source, shift);
        const auto mask = rotate_mask(ins.mask_begin, ins.mask_end);
        const auto selected = static_cast<std::uint32_t>(rotated & mask);
        const auto value = ins.opcode == Opcode::rotate_left_word_mask_insert
                               ? static_cast<std::uint32_t>(
                                     (static_cast<std::uint32_t>(state.gpr[ins.ra]) & ~mask) | selected)
                               : selected;
        state.gpr[ins.ra] = value;
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::rotate_left_doubleword_clear_left:
    case Opcode::rotate_left_doubleword_clear_right:
    case Opcode::rotate_left_doubleword_clear:
    case Opcode::rotate_left_doubleword_mask_insert:
    case Opcode::rotate_left_doubleword_variable_clear_left:
    case Opcode::rotate_left_doubleword_variable_clear_right: {
        const bool variable_shift =
            ins.opcode == Opcode::rotate_left_doubleword_variable_clear_left ||
            ins.opcode == Opcode::rotate_left_doubleword_variable_clear_right;
        const auto shift = variable_shift ? static_cast<int>(state.gpr[ins.rb] & 0x3fU)
                                          : static_cast<int>(ins.shift);
        const auto rotated = std::rotl(state.gpr[ins.rt], shift);
        std::uint8_t begin = 0;
        std::uint8_t end = 63;
        if (ins.opcode == Opcode::rotate_left_doubleword_clear_left ||
            ins.opcode == Opcode::rotate_left_doubleword_variable_clear_left) {
            begin = ins.mask_begin;
        } else if (ins.opcode == Opcode::rotate_left_doubleword_clear_right ||
                   ins.opcode == Opcode::rotate_left_doubleword_variable_clear_right) {
            end = ins.mask_end;
        } else {
            begin = ins.mask_begin;
            end = static_cast<std::uint8_t>(63U - shift);
        }
        const auto mask = rotate_mask_doubleword(begin, end);
        const auto selected = rotated & mask;
        const auto value = ins.opcode == Opcode::rotate_left_doubleword_mask_insert
                               ? (state.gpr[ins.ra] & ~mask) | selected
                               : selected;
        state.gpr[ins.ra] = value;
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::count_leading_zeros_word: {
        const auto source = static_cast<std::uint32_t>(state.gpr[ins.rt]);
        const auto value = static_cast<std::uint64_t>(std::countl_zero(source));
        state.gpr[ins.ra] = value;
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::count_leading_zeros_doubleword: {
        const auto value = static_cast<std::uint64_t>(std::countl_zero(state.gpr[ins.rt]));
        state.gpr[ins.ra] = value;
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::shift_left_word:
    case Opcode::shift_right_word: {
        const auto source = static_cast<std::uint32_t>(state.gpr[ins.rt]);
        const auto amount = static_cast<unsigned>(state.gpr[ins.rb] & 0x3fU);
        std::uint32_t value = 0;
        if (amount < 32U) {
            value = ins.opcode == Opcode::shift_left_word ? source << amount : source >> amount;
        }
        state.gpr[ins.ra] = value;
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::shift_right_algebraic_word: {
        const auto source = static_cast<std::uint32_t>(state.gpr[ins.rt]);
        const auto signed_source = std::bit_cast<std::int32_t>(source);
        const auto amount = static_cast<unsigned>(state.gpr[ins.rb] & 0x3fU);
        const bool negative = signed_source < 0;
        const auto shifted = amount >= 32U ? (negative ? -1 : 0) : (signed_source >> amount);
        bool carry = false;
        if (amount >= 32U) {
            carry = negative && source != 0U;
        } else if (amount != 0U) {
            const auto discarded_mask = (1U << amount) - 1U;
            carry = negative && (source & discarded_mask) != 0U;
        }
        const auto value = static_cast<std::uint64_t>(static_cast<std::int64_t>(shifted));
        state.gpr[ins.ra] = value;
        set_xer_carry(state, carry);
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::shift_right_algebraic_word_immediate: {
        const auto source = static_cast<std::uint32_t>(state.gpr[ins.rt]);
        const auto signed_source = std::bit_cast<std::int32_t>(source);
        const auto amount = static_cast<unsigned>(ins.shift);
        const auto shifted = signed_source >> amount;
        const bool negative = signed_source < 0;
        const auto discarded_mask = amount == 0U ? 0U : (1U << amount) - 1U;
        const bool carry = negative && (source & discarded_mask) != 0U;
        const auto value = static_cast<std::uint64_t>(static_cast<std::int64_t>(shifted));
        state.gpr[ins.ra] = value;
        set_xer_carry(state, carry);
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::shift_left_doubleword:
    case Opcode::shift_right_doubleword: {
        const auto source = state.gpr[ins.rt];
        const auto amount = static_cast<unsigned>(state.gpr[ins.rb] & 0x7fU);
        std::uint64_t value = 0;
        if (amount < 64U) {
            value = ins.opcode == Opcode::shift_left_doubleword ? source << amount : source >> amount;
        }
        state.gpr[ins.ra] = value;
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::shift_right_algebraic_doubleword: {
        const auto source = state.gpr[ins.rt];
        const auto amount = static_cast<unsigned>(state.gpr[ins.rb] & 0x7fU);
        const bool negative = (source & 0x8000000000000000ULL) != 0U;
        std::uint64_t value = source;
        bool carry = false;
        if (amount >= 64U) {
            value = negative ? std::numeric_limits<std::uint64_t>::max() : 0U;
            carry = negative;
        } else if (amount != 0U) {
            value = source >> amount;
            if (negative) value |= std::numeric_limits<std::uint64_t>::max() << (64U - amount);
            const auto discarded_mask = (std::uint64_t{1} << amount) - 1U;
            carry = negative && (source & discarded_mask) != 0U;
        }
        state.gpr[ins.ra] = value;
        set_xer_carry(state, carry);
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::shift_right_algebraic_doubleword_immediate: {
        const auto source = state.gpr[ins.rt];
        const auto amount = static_cast<unsigned>(ins.shift);
        const bool negative = (source & 0x8000000000000000ULL) != 0U;
        std::uint64_t value = source;
        bool carry = false;
        if (amount != 0U) {
            value = source >> amount;
            if (negative) value |= std::numeric_limits<std::uint64_t>::max() << (64U - amount);
            const auto discarded_mask = (std::uint64_t{1} << amount) - 1U;
            carry = negative && (source & discarded_mask) != 0U;
        }
        state.gpr[ins.ra] = value;
        set_xer_carry(state, carry);
        if (ins.record) set_cr0(state, value);
        break;
    }
    case Opcode::add:
    case Opcode::addc:
    case Opcode::adde: {
        const bool carry_in = ins.opcode == Opcode::adde && (state.xer & kXerCarry) != 0U;
        const auto result = add_values(state.gpr[ins.ra], state.gpr[ins.rb], carry_in);
        state.gpr[ins.rt] = result.value;
        if (ins.opcode == Opcode::addc || ins.opcode == Opcode::adde) {
            set_xer_carry(state, result.carry);
        }
        if (ins.overflow_enable) set_xer_overflow(state, result.overflow);
        if (ins.record) set_cr0(state, state.gpr[ins.rt]);
        break;
    }
    case Opcode::mullw: {
        const auto left = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(state.gpr[ins.ra]));
        const auto right = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(state.gpr[ins.rb]));
        const auto product = static_cast<std::int64_t>(left) * static_cast<std::int64_t>(right);
        const auto low_word = static_cast<std::uint32_t>(product);
        // PowerPC64 defines only RT[32:63] for mullw. Clear RT[0:31] as
        // a deterministic policy for the architecturally undefined upper half.
        state.gpr[ins.rt] = low_word;
        if (ins.overflow_enable) {
            const bool overflow = product < std::numeric_limits<std::int32_t>::min() ||
                                  product > std::numeric_limits<std::int32_t>::max();
            set_xer_overflow(state, overflow);
        }
        if (ins.record) {
            const auto signed_result = std::bit_cast<std::int32_t>(low_word);
            set_cr0(state, static_cast<std::uint64_t>(static_cast<std::int64_t>(signed_result)));
        }
        break;
    }
    case Opcode::mulld: {
        const auto left = std::bit_cast<std::int64_t>(state.gpr[ins.ra]);
        const auto right = std::bit_cast<std::int64_t>(state.gpr[ins.rb]);
        const auto result = state.gpr[ins.ra] * state.gpr[ins.rb];
        state.gpr[ins.rt] = result;
        if (ins.overflow_enable) set_xer_overflow(state, signed_multiply_overflow(left, right));
        if (ins.record) set_cr0(state, result);
        break;
    }
    case Opcode::mulhw:
    case Opcode::mulhwu: {
        const auto left_bits = static_cast<std::uint32_t>(state.gpr[ins.ra]);
        const auto right_bits = static_cast<std::uint32_t>(state.gpr[ins.rb]);
        std::uint32_t high_word = 0;
        if (ins.opcode == Opcode::mulhw) {
            const auto left = std::bit_cast<std::int32_t>(left_bits);
            const auto right = std::bit_cast<std::int32_t>(right_bits);
            const auto product = static_cast<std::int64_t>(left) * right;
            high_word = static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32U);
        } else {
            const auto product = static_cast<std::uint64_t>(left_bits) * right_bits;
            high_word = static_cast<std::uint32_t>(product >> 32U);
        }
        // As with other PowerPC64 word operations, the upper destination
        // half is architecturally undefined; clear it deterministically here.
        state.gpr[ins.rt] = high_word;
        if (ins.record) {
            const auto signed_result = std::bit_cast<std::int32_t>(high_word);
            set_cr0(state, static_cast<std::uint64_t>(static_cast<std::int64_t>(signed_result)));
        }
        break;
    }
    case Opcode::mulhd:
    case Opcode::mulhdu: {
        const auto result = ins.opcode == Opcode::mulhd
                                ? multiply_high_signed(state.gpr[ins.ra], state.gpr[ins.rb])
                                : multiply_high_unsigned(state.gpr[ins.ra], state.gpr[ins.rb]);
        state.gpr[ins.rt] = result;
        if (ins.record) set_cr0(state, result);
        break;
    }
    case Opcode::divw:
    case Opcode::divwu:
    case Opcode::divd:
    case Opcode::divdu: {
        const bool is_word_division = ins.opcode == Opcode::divw || ins.opcode == Opcode::divwu;
        const bool signed_division = ins.opcode == Opcode::divw || ins.opcode == Opcode::divd;
        bool invalid = false;
        std::uint64_t result = 0;
        if (is_word_division) {
            const auto dividend_bits = static_cast<std::uint32_t>(state.gpr[ins.ra]);
            const auto divisor_bits = static_cast<std::uint32_t>(state.gpr[ins.rb]);
            invalid = divisor_bits == 0U;
            if (signed_division) {
                const auto dividend = std::bit_cast<std::int32_t>(dividend_bits);
                const auto divisor = std::bit_cast<std::int32_t>(divisor_bits);
                invalid = invalid || (dividend == std::numeric_limits<std::int32_t>::min() &&
                                      divisor == -1);
                if (!invalid) {
                    const auto quotient = static_cast<std::int32_t>(dividend / divisor);
                    // PowerPC leaves RT[0:31] undefined for word division. This
                    // interpreter deterministically clears those bits and writes
                    // the architecturally defined low word.
                    result = static_cast<std::uint32_t>(quotient);
                }
            } else if (!invalid) {
                const auto quotient = dividend_bits / divisor_bits;
                result = quotient;
            }
        } else if (signed_division) {
            const auto dividend = std::bit_cast<std::int64_t>(state.gpr[ins.ra]);
            const auto divisor = std::bit_cast<std::int64_t>(state.gpr[ins.rb]);
            invalid = divisor == 0 ||
                      (dividend == std::numeric_limits<std::int64_t>::min() && divisor == -1);
            if (!invalid) result = static_cast<std::uint64_t>(dividend / divisor);
        } else {
            const auto divisor = state.gpr[ins.rb];
            invalid = divisor == 0U;
            if (!invalid) result = state.gpr[ins.ra] / divisor;
        }

        // The ISA defines the destination as undefined for divide-by-zero and
        // signed-minimum divided by -1. Choose zero as a stable diagnostic result.
        state.gpr[ins.rt] = result;
        if (ins.overflow_enable) set_xer_overflow(state, invalid);
        if (ins.record) {
            // Word forms compare the 32-bit result as signed for CR0, including
            // divwu, whose quotient itself is unsigned.
            const auto cr_value = is_word_division
                                      ? static_cast<std::uint64_t>(static_cast<std::int64_t>(
                                            std::bit_cast<std::int32_t>(
                                                static_cast<std::uint32_t>(result))))
                                      : result;
            set_cr0(state, cr_value);
        }
        break;
    }
    case Opcode::addic: {
        const auto result = add_values(state.gpr[ins.ra], static_cast<std::uint64_t>(ins.immediate), false);
        state.gpr[ins.rt] = result.value;
        set_xer_carry(state, result.carry);
        if (ins.record) set_cr0(state, result.value);
        break;
    }
    case Opcode::addze: {
        const bool carry_in = (state.xer & kXerCarry) != 0U;
        const auto result = add_values(state.gpr[ins.ra], 0U, carry_in);
        state.gpr[ins.rt] = result.value;
        set_xer_carry(state, result.carry);
        if (ins.overflow_enable) set_xer_overflow(state, result.overflow);
        if (ins.record) set_cr0(state, result.value);
        break;
    }
    case Opcode::addme: {
        const bool carry_in = (state.xer & kXerCarry) != 0U;
        const auto result = add_values(state.gpr[ins.ra],
                                       std::numeric_limits<std::uint64_t>::max(), carry_in);
        state.gpr[ins.rt] = result.value;
        set_xer_carry(state, result.carry);
        if (ins.overflow_enable) set_xer_overflow(state, result.overflow);
        if (ins.record) set_cr0(state, result.value);
        break;
    }
    case Opcode::neg: {
        const auto operand = state.gpr[ins.ra];
        const auto result = std::uint64_t{0} - operand;
        state.gpr[ins.rt] = result;
        if (ins.overflow_enable) {
            const bool overflow = operand == 0x8000000000000000ULL;
            set_xer_overflow(state, overflow);
        }
        if (ins.record) set_cr0(state, result);
        break;
    }
    case Opcode::subf:
    case Opcode::subfc:
    case Opcode::subfe:
    case Opcode::subfic: {
        const auto minuend = ins.opcode == Opcode::subfic
                                 ? static_cast<std::uint64_t>(ins.immediate)
                                 : state.gpr[ins.rb];
        const auto subtrahend = state.gpr[ins.ra];
        const bool carry_in = ins.opcode != Opcode::subfe || (state.xer & kXerCarry) != 0U;
        const auto result = add_values(minuend, ~subtrahend, carry_in);
        state.gpr[ins.rt] = result.value;
        if (ins.opcode != Opcode::subf) set_xer_carry(state, result.carry);
        if (ins.overflow_enable) set_xer_overflow(state, result.overflow);
        if (ins.record) set_cr0(state, result.value);
        break;
    }
    case Opcode::subfze: {
        const bool carry_in = (state.xer & kXerCarry) != 0U;
        const auto result = add_values(~state.gpr[ins.ra], 0U, carry_in);
        state.gpr[ins.rt] = result.value;
        set_xer_carry(state, result.carry);
        if (ins.overflow_enable) set_xer_overflow(state, result.overflow);
        if (ins.record) set_cr0(state, result.value);
        break;
    }
    case Opcode::subfme: {
        const bool carry_in = (state.xer & kXerCarry) != 0U;
        const auto result = add_values(~state.gpr[ins.ra],
                                       std::numeric_limits<std::uint64_t>::max(), carry_in);
        state.gpr[ins.rt] = result.value;
        set_xer_carry(state, result.carry);
        if (ins.overflow_enable) set_xer_overflow(state, result.overflow);
        if (ins.record) set_cr0(state, result.value);
        break;
    }
    case Opcode::cmp: {
        bool less = false;
        bool greater = false;
        if (ins.wide) {
            const auto left = std::bit_cast<std::int64_t>(state.gpr[ins.ra]);
            const auto right = std::bit_cast<std::int64_t>(state.gpr[ins.rb]);
            less = left < right;
            greater = left > right;
        } else {
            const auto left = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(state.gpr[ins.ra]));
            const auto right = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(state.gpr[ins.rb]));
            less = left < right;
            greater = left > right;
        }
        set_cr_field(state, ins.bf, compare_field(less, greater,
                                                  (state.xer & 0x80000000U) != 0U));
        break;
    }
    case Opcode::cmpl: {
        bool less = false;
        bool greater = false;
        if (ins.wide) {
            less = state.gpr[ins.ra] < state.gpr[ins.rb];
            greater = state.gpr[ins.ra] > state.gpr[ins.rb];
        } else {
            const auto left = static_cast<std::uint32_t>(state.gpr[ins.ra]);
            const auto right = static_cast<std::uint32_t>(state.gpr[ins.rb]);
            less = left < right;
            greater = left > right;
        }
        set_cr_field(state, ins.bf, compare_field(less, greater,
                                                  (state.xer & 0x80000000U) != 0U));
        break;
    }
    case Opcode::cmpi: {
        bool less = false;
        bool greater = false;
        if (ins.wide) {
            const auto left = std::bit_cast<std::int64_t>(state.gpr[ins.ra]);
            less = left < ins.immediate;
            greater = left > ins.immediate;
        } else {
            const auto left = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(state.gpr[ins.ra]));
            const auto right = static_cast<std::int32_t>(ins.immediate);
            less = left < right;
            greater = left > right;
        }
        set_cr_field(state, ins.bf, compare_field(less, greater,
                                                  (state.xer & 0x80000000U) != 0U));
        break;
    }
    case Opcode::cmpli: {
        bool less = false;
        bool greater = false;
        if (ins.wide) {
            less = state.gpr[ins.ra] < static_cast<std::uint64_t>(ins.immediate);
            greater = state.gpr[ins.ra] > static_cast<std::uint64_t>(ins.immediate);
        } else {
            const auto left = static_cast<std::uint32_t>(state.gpr[ins.ra]);
            const auto right = static_cast<std::uint32_t>(ins.immediate);
            less = left < right;
            greater = left > right;
        }
        set_cr_field(state, ins.bf, compare_field(less, greater,
                                                  (state.xer & 0x80000000U) != 0U));
        break;
    }
    case Opcode::move_from_condition_register:
        state.gpr[ins.rt] = state.cr;
        break;
    case Opcode::move_condition_register_field: {
        const auto source_shift = static_cast<unsigned>(28U - 4U * ins.bfa);
        const auto source_field = (state.cr >> source_shift) & 0xfU;
        set_cr_field(state, ins.bf, source_field);
        break;
    }
    case Opcode::condition_register_logical: {
        const auto bit_mask = [](std::uint8_t bit) {
            return std::uint32_t{1} << (31U - bit);
        };
        const bool a = (state.cr & bit_mask(ins.ra)) != 0U;
        const bool b = (state.cr & bit_mask(ins.rb)) != 0U;
        bool value = false;
        switch (ins.cr_logical_operation) {
        case CrLogicalOperation::invalid:
            return internal_fault("invalid decoded condition-register logical operation");
        case CrLogicalOperation::and_: value = a && b; break;
        case CrLogicalOperation::and_complement: value = a && !b; break;
        case CrLogicalOperation::equivalent: value = a == b; break;
        case CrLogicalOperation::nand: value = !(a && b); break;
        case CrLogicalOperation::nor: value = !(a || b); break;
        case CrLogicalOperation::or_: value = a || b; break;
        case CrLogicalOperation::or_complement: value = a || !b; break;
        case CrLogicalOperation::xor_: value = a != b; break;
        }
        const auto target_mask = bit_mask(ins.rt);
        state.cr = value ? (state.cr | target_mask) : (state.cr & ~target_mask);
        break;
    }
    case Opcode::move_to_condition_register_fields: {
        const auto source = static_cast<std::uint32_t>(state.gpr[ins.rt]);
        for (std::uint8_t field = 0; field < 8U; ++field) {
            const auto field_bit = static_cast<std::uint8_t>(0x80U >> field);
            if ((ins.field_mask & field_bit) == 0U) continue;
            const auto shift = static_cast<unsigned>(28U - 4U * field);
            const auto cr_mask = 0xfU << shift;
            const auto field_value = (source >> shift) & 0xfU;
            state.cr = (state.cr & ~cr_mask) | (field_value << shift);
        }
        break;
    }
    case Opcode::move_condition_register_from_xer: {
        const auto xer_field = (state.xer >> 28U) & 0xfU;
        set_cr_field(state, ins.bf, xer_field);
        state.xer &= 0x0fffffffU;
        break;
    }
    case Opcode::move_from_special_register:
        if ((ins.spr == 26U || ins.spr == 27U) &&
            (state.msr & powerpc_bit(49U)) != 0U) {
            return take_program_exception(state, FaultKind::privileged_instruction, 45U, word,
                                           "privileged mfspr access to SRR0/SRR1");
        }
        switch (ins.spr) {
        case 1U: state.gpr[ins.rt] = state.xer; break;
        case 8U: state.gpr[ins.rt] = state.lr; break;
        case 9U: state.gpr[ins.rt] = state.ctr; break;
        case 26U: state.gpr[ins.rt] = state.srr0; break;
        case 27U: state.gpr[ins.rt] = state.srr1; break;
        default: return internal_fault("unsupported mfspr register");
        }
        break;
    case Opcode::move_to_special_register:
        if ((ins.spr == 26U || ins.spr == 27U) &&
            (state.msr & powerpc_bit(49U)) != 0U) {
            return take_program_exception(state, FaultKind::privileged_instruction, 45U, word,
                                           "privileged mtspr access to SRR0/SRR1");
        }
        switch (ins.spr) {
        case 1U: state.xer = static_cast<std::uint32_t>(state.gpr[ins.rt]); break;
        case 8U: state.lr = state.gpr[ins.rt]; break;
        case 9U: state.ctr = state.gpr[ins.rt]; break;
        case 26U: state.srr0 = state.gpr[ins.rt]; break;
        case 27U: state.srr1 = state.gpr[ins.rt]; break;
        default: return internal_fault("unsupported mtspr register");
        }
        break;
    case Opcode::lbz: {
        std::uint8_t value = 0;
        const auto offset = ins.indexed ? state.gpr[ins.rb] : static_cast<std::uint64_t>(ins.immediate);
        const auto address = base + offset;
        if (!memory.read_u8(address, value)) return data_fault(address, 1U, "lbz address fault");
        state.gpr[ins.rt] = value;
        if (ins.update) state.gpr[ins.ra] = address;
        break;
    }
    case Opcode::lhz:
    case Opcode::lha: {
        std::uint16_t value = 0;
        const auto offset = ins.indexed ? state.gpr[ins.rb] : static_cast<std::uint64_t>(ins.immediate);
        const auto address = base + offset;
        if (!memory.read_u16_be(address, value)) {
            return data_fault(address, 2U, "halfword load alignment or address fault");
        }
        state.gpr[ins.rt] = ins.opcode == Opcode::lha
                                ? static_cast<std::uint64_t>(static_cast<std::int64_t>(
                                      std::bit_cast<std::int16_t>(value)))
                                : static_cast<std::uint64_t>(value);
        if (ins.update) state.gpr[ins.ra] = address;
        break;
    }
    case Opcode::stb: {
        const auto offset = ins.indexed ? state.gpr[ins.rb] : static_cast<std::uint64_t>(ins.immediate);
        const auto address = base + offset;
        if (!memory.write_u8(address, static_cast<std::uint8_t>(state.gpr[ins.rt]))) {
            return data_fault(address, 1U, "stb address fault");
        }
        if (ins.update) state.gpr[ins.ra] = address;
        break;
    }
    case Opcode::sth: {
        const auto offset = ins.indexed ? state.gpr[ins.rb] : static_cast<std::uint64_t>(ins.immediate);
        const auto address = base + offset;
        if (!memory.write_u16_be(address, static_cast<std::uint16_t>(state.gpr[ins.rt]))) {
            return data_fault(address, 2U, "sth alignment or address fault");
        }
        if (ins.update) state.gpr[ins.ra] = address;
        break;
    }
    case Opcode::lwz: {
        const auto offset = ins.indexed ? state.gpr[ins.rb] : static_cast<std::uint64_t>(ins.immediate);
        const auto address = base + offset;
        std::uint32_t value = 0;
        if (!memory.read_u32_be(address, value)) {
            return data_fault(address, 4U, "lwz alignment or address fault");
        }
        state.gpr[ins.rt] = value;
        if (ins.update) state.gpr[ins.ra] = address;
        break;
    }
    case Opcode::ld: {
        const auto offset = ins.indexed ? state.gpr[ins.rb] : static_cast<std::uint64_t>(ins.immediate);
        const auto address = base + offset;
        std::uint64_t value = 0;
        if (!memory.read_u64_be(address, value)) {
            return data_fault(address, 8U, "ld alignment or address fault");
        }
        state.gpr[ins.rt] = value;
        if (ins.update) state.gpr[ins.ra] = address;
        break;
    }
    case Opcode::lhbrx: {
        const auto address = base + state.gpr[ins.rb];
        std::uint16_t value = 0;
        if (!memory.read_u16_be(address, value)) {
            return data_fault(address, 2U, "lhbrx alignment or address fault");
        }
        const auto reversed = static_cast<std::uint16_t>((value >> 8U) | (value << 8U));
        state.gpr[ins.rt] = reversed;
        break;
    }
    case Opcode::lwbrx: {
        const auto address = base + state.gpr[ins.rb];
        std::uint32_t value = 0;
        if (!memory.read_u32_be(address, value)) {
            return data_fault(address, 4U, "lwbrx alignment or address fault");
        }
        const auto reversed = ((value & 0x000000ffU) << 24U) |
                              ((value & 0x0000ff00U) << 8U) |
                              ((value & 0x00ff0000U) >> 8U) |
                              ((value & 0xff000000U) >> 24U);
        state.gpr[ins.rt] = reversed;
        break;
    }
    case Opcode::load_word_and_reserve_indexed: {
        state.reservation_valid = false;
        const auto address = base + state.gpr[ins.rb];
        std::uint32_t value = 0U;
        std::uint64_t reserved_address = 0U;
        std::uint64_t granule = 0U;
        std::uint64_t generation = 0U;
        if (!memory.read_reserve_u32_be(address, value, reserved_address, granule,
                                        generation)) {
            return data_fault(address, 4U, "lwarx alignment or address fault");
        }
        state.gpr[ins.rt] = value;
        state.reservation_address = reserved_address;
        state.reservation_granule = granule;
        state.reservation_generation = generation;
        state.reservation_valid = true;
        break;
    }
    case Opcode::load_doubleword_and_reserve_indexed: {
        state.reservation_valid = false;
        const auto address = base + state.gpr[ins.rb];
        std::uint64_t value = 0U;
        std::uint64_t reserved_address = 0U;
        std::uint64_t granule = 0U;
        std::uint64_t generation = 0U;
        if (!memory.read_reserve_u64_be(address, value, reserved_address, granule,
                                        generation)) {
            return data_fault(address, 8U, "ldarx alignment or address fault");
        }
        state.gpr[ins.rt] = value;
        state.reservation_address = reserved_address;
        state.reservation_granule = granule;
        state.reservation_generation = generation;
        state.reservation_valid = true;
        break;
    }
    case Opcode::store_word_conditional_indexed: {
        const auto address = base + state.gpr[ins.rb];
        bool stored = false;
        const auto reserved_address = state.reservation_valid
                                          ? state.reservation_address
                                          : std::numeric_limits<std::uint64_t>::max();
        const auto granule = state.reservation_valid ? state.reservation_granule
                                                     : std::numeric_limits<std::uint64_t>::max();
        const auto generation = state.reservation_generation;
        state.reservation_valid = false;
        if (!memory.write_conditional_u32_be(address,
                                             static_cast<std::uint32_t>(state.gpr[ins.rt]),
                                             reserved_address, granule, generation, stored)) {
            return data_fault(address, 4U, "stwcx. alignment or address fault");
        }
        const auto summary_overflow = (state.xer & kXerSummaryOverflow) != 0U;
        const auto condition_field = (stored ? 0x2U : 0U) |
                                     (summary_overflow ? 0x1U : 0U);
        set_cr_field(state, 0U, condition_field);
        break;
    }
    case Opcode::store_doubleword_conditional_indexed: {
        const auto address = base + state.gpr[ins.rb];
        bool stored = false;
        const auto reserved_address = state.reservation_valid
                                          ? state.reservation_address
                                          : std::numeric_limits<std::uint64_t>::max();
        const auto granule = state.reservation_valid ? state.reservation_granule
                                                     : std::numeric_limits<std::uint64_t>::max();
        const auto generation = state.reservation_generation;
        state.reservation_valid = false;
        if (!memory.write_conditional_u64_be(address, state.gpr[ins.rt], reserved_address,
                                             granule, generation, stored)) {
            return data_fault(address, 8U, "stdcx. alignment or address fault");
        }
        const auto summary_overflow = (state.xer & kXerSummaryOverflow) != 0U;
        const auto condition_field = (stored ? 0x2U : 0U) |
                                     (summary_overflow ? 0x1U : 0U);
        set_cr_field(state, 0U, condition_field);
        break;
    }
    case Opcode::stw: {
        const auto offset = ins.indexed ? state.gpr[ins.rb] : static_cast<std::uint64_t>(ins.immediate);
        const auto address = base + offset;
        if (!memory.write_u32_be(address, static_cast<std::uint32_t>(state.gpr[ins.rt]))) {
            return data_fault(address, 4U, "stw alignment or address fault");
        }
        if (ins.update) state.gpr[ins.ra] = address;
        break;
    }
    case Opcode::lfs:
    case Opcode::lfd: {
        const auto offset = ins.indexed ? state.gpr[ins.rb]
                                        : static_cast<std::uint64_t>(ins.immediate);
        const auto address = base + offset;
        if (ins.opcode == Opcode::lfs) {
            std::uint32_t bits = 0;
            if (!memory.read_u32_be(address, bits)) {
                return data_fault(address, 4U, "lfs alignment or address fault");
            }
            const auto value = static_cast<double>(std::bit_cast<float>(bits));
            state.fpr[ins.rt] = std::bit_cast<std::uint64_t>(value);
        } else {
            std::uint64_t bits = 0;
            if (!memory.read_u64_be(address, bits)) {
                return data_fault(address, 8U, "lfd alignment or address fault");
            }
            state.fpr[ins.rt] = bits;
        }
        if (ins.update) state.gpr[ins.ra] = address;
        break;
    }
    case Opcode::stfs:
    case Opcode::stfd: {
        const auto offset = ins.indexed ? state.gpr[ins.rb]
                                        : static_cast<std::uint64_t>(ins.immediate);
        const auto address = base + offset;
        if (ins.opcode == Opcode::stfs) {
            const auto value = std::bit_cast<double>(state.fpr[ins.rt]);
            const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(value));
            if (!memory.write_u32_be(address, bits)) {
                return data_fault(address, 4U, "stfs alignment or address fault");
            }
        } else if (!memory.write_u64_be(address, state.fpr[ins.rt])) {
            return data_fault(address, 8U, "stfd alignment or address fault");
        }
        if (ins.update) state.gpr[ins.ra] = address;
        break;
    }
    case Opcode::stfiwx: {
        const auto address = base + state.gpr[ins.rb];
        if (!memory.write_u32_be(address, static_cast<std::uint32_t>(state.fpr[ins.rt]))) {
            return data_fault(address, 4U, "stfiwx alignment or address fault");
        }
        break;
    }
    case Opcode::std: {
        const auto offset = ins.indexed ? state.gpr[ins.rb] : static_cast<std::uint64_t>(ins.immediate);
        const auto address = base + offset;
        if (!memory.write_u64_be(address, state.gpr[ins.rt])) {
            return data_fault(address, 8U, "std alignment or address fault");
        }
        if (ins.update) state.gpr[ins.ra] = address;
        break;
    }
    case Opcode::sthbrx: {
        const auto address = base + state.gpr[ins.rb];
        const auto value = static_cast<std::uint16_t>(state.gpr[ins.rt]);
        const auto reversed = static_cast<std::uint16_t>((value >> 8U) | (value << 8U));
        if (!memory.write_u16_be(address, reversed)) {
            return data_fault(address, 2U, "sthbrx alignment or address fault");
        }
        break;
    }
    case Opcode::stwbrx: {
        const auto address = base + state.gpr[ins.rb];
        const auto value = static_cast<std::uint32_t>(state.gpr[ins.rt]);
        const auto reversed = ((value & 0x000000ffU) << 24U) |
                              ((value & 0x0000ff00U) << 8U) |
                              ((value & 0x00ff0000U) >> 8U) |
                              ((value & 0xff000000U) >> 24U);
        if (!memory.write_u32_be(address, reversed)) {
            return data_fault(address, 4U, "stwbrx alignment or address fault");
        }
        break;
    }
    case Opcode::load_string_word_immediate:
    case Opcode::load_string_word_indexed: {
        const auto byte_count = ins.opcode == Opcode::load_string_word_immediate
                                    ? static_cast<std::size_t>(ins.byte_count)
                                    : static_cast<std::size_t>(state.xer & kXerByteCountMask);
        if (byte_count == 0U) break;
        const auto register_count = (byte_count + 3U) / 4U;
        if (ins.opcode == Opcode::load_string_word_indexed &&
            (register_in_sequence(ins.ra, ins.rt, register_count) ||
             register_in_sequence(ins.rb, ins.rt, register_count))) {
            return fault(FaultKind::unsupported_instruction, old_pc,
                         "lswx with an address register in the destination range has undefined results",
                         word);
        }

        const auto address = ins.opcode == Opcode::load_string_word_immediate
                                 ? base
                                 : base + state.gpr[ins.rb];
        if (address > std::numeric_limits<std::uint64_t>::max() - (byte_count - 1U)) {
            return data_fault(address, 1U, "load string address range overflows");
        }
        std::array<std::uint32_t, 32> values{};
        for (std::size_t index = 0U; index < byte_count; ++index) {
            std::uint8_t byte = 0U;
            if (!memory.read_u8(address + index, byte)) {
                return data_fault(address + index, 1U, "load string address fault");
            }
            const auto shift = static_cast<unsigned>(24U - (index % 4U) * 8U);
            values[index / 4U] |= static_cast<std::uint32_t>(byte) << shift;
        }
        for (std::size_t index = 0U; index < register_count; ++index) {
            state.gpr[(static_cast<std::size_t>(ins.rt) + index) & 0x1fU] = values[index];
        }
        break;
    }
    case Opcode::store_string_word_immediate:
    case Opcode::store_string_word_indexed: {
        const auto byte_count = ins.opcode == Opcode::store_string_word_immediate
                                    ? static_cast<std::size_t>(ins.byte_count)
                                    : static_cast<std::size_t>(state.xer & kXerByteCountMask);
        if (byte_count == 0U) break;
        const auto address = ins.opcode == Opcode::store_string_word_immediate
                                 ? base
                                 : base + state.gpr[ins.rb];
        std::array<std::byte, 127> bytes{};
        for (std::size_t index = 0U; index < byte_count; ++index) {
            const auto source_register =
                (static_cast<std::size_t>(ins.rt) + index / 4U) & 0x1fU;
            const auto value = static_cast<std::uint32_t>(state.gpr[source_register]);
            const auto shift = static_cast<unsigned>(24U - (index % 4U) * 8U);
            bytes[index] = static_cast<std::byte>((value >> shift) & 0xffU);
        }
        if (!memory.write_bytes(address, std::span<const std::byte>(bytes.data(), byte_count))) {
            return data_fault(address, 1U, "store string address fault");
        }
        break;
    }
    case Opcode::lmw: {
        const auto address = base + static_cast<std::uint64_t>(ins.immediate);
        const auto count = static_cast<std::size_t>(32U - ins.rt);
        const auto byte_count = count * sizeof(std::uint32_t);
        if ((address & 3U) != 0U) {
            return data_fault(address, 4U, "lmw alignment or address fault");
        }
        if (address > std::numeric_limits<std::uint64_t>::max() - (byte_count - 1U)) {
            return data_fault(address, 1U, "lmw address range overflows");
        }
        std::array<std::byte, 32U * sizeof(std::uint32_t)> bytes{};
        std::uint64_t fault_address = address;
        if (!memory.read_bytes(address, std::span<std::byte>(bytes.data(), byte_count),
                               &fault_address)) {
            return data_fault(fault_address, 1U, "lmw guest address fault");
        }
        std::array<std::uint32_t, 32> values{};
        for (std::size_t index = 0U; index < byte_count; ++index) {
            const auto shift = static_cast<unsigned>(24U - (index % 4U) * 8U);
            values[index / 4U] |=
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index])) << shift;
        }
        for (std::size_t i = 0; i < count; ++i) {
            state.gpr[static_cast<std::size_t>(ins.rt) + i] = values[i];
        }
        break;
    }
    case Opcode::stmw: {
        const auto address = base + static_cast<std::uint64_t>(ins.immediate);
        const auto count = static_cast<std::size_t>(32U - ins.rt);
        const auto byte_count = count * sizeof(std::uint32_t);
        if ((address & 3U) != 0U) {
            return data_fault(address, 4U, "stmw alignment or address fault");
        }
        if (address > std::numeric_limits<std::uint64_t>::max() - (byte_count - 1U)) {
            return data_fault(address, 1U, "stmw address range overflows");
        }
        std::array<std::byte, 32U * sizeof(std::uint32_t)> bytes{};
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = static_cast<std::uint32_t>(state.gpr[static_cast<std::size_t>(ins.rt) + i]);
            for (std::size_t byte_index = 0U; byte_index < sizeof(std::uint32_t); ++byte_index) {
                const auto shift = static_cast<unsigned>(24U - byte_index * 8U);
                bytes[i * sizeof(std::uint32_t) + byte_index] =
                    static_cast<std::byte>((value >> shift) & 0xffU);
            }
        }
        if (!memory.write_bytes(address, std::span<const std::byte>(bytes.data(), byte_count))) {
            return data_fault(address, 1U, "stmw guest address fault");
        }
        break;
    }
    case Opcode::branch:
        if (ins.link) state.lr = old_pc + 4U;
        next_pc = ins.absolute ? static_cast<std::uint64_t>(ins.immediate)
                               : old_pc + static_cast<std::uint64_t>(ins.immediate);
        break;
    case Opcode::branch_conditional: {
        if (ins.link) state.lr = old_pc + 4U;
        if (evaluate_branch_condition(state, ins.bo, ins.bi)) {
            next_pc = ins.absolute ? static_cast<std::uint64_t>(ins.immediate)
                                   : old_pc + static_cast<std::uint64_t>(ins.immediate);
        }
        break;
    }
    case Opcode::branch_lr:
    case Opcode::branch_ctr: {
        const auto target = (ins.opcode == Opcode::branch_lr ? state.lr : state.ctr) & ~3ULL;
        if (ins.link) state.lr = old_pc + 4U;
        if (evaluate_branch_condition(state, ins.bo, ins.bi)) next_pc = target;
        break;
    }
    case Opcode::return_from_interrupt_doubleword:
        if ((state.msr & powerpc_bit(49U)) != 0U) {
            return take_program_exception(state, FaultKind::privileged_instruction, 45U, word,
                                           "privileged rfid instruction");
        }
        state.msr = restore_msr_from_srr1(state.msr, state.srr1);
        if ((state.msr & powerpc_bit(0U)) != 0U) {
            next_pc = state.srr0 & ~std::uint64_t{3};
        } else {
            next_pc = state.srr0 & 0xfffffffcULL;
        }
        break;
    }
    state.pc = next_pc;
    return {};
}

StepResult Interpreter::run(CpuState& state, memory::GuestMemory& memory,
                            std::size_t instruction_limit) const {
    for (std::size_t i = 0; i < instruction_limit; ++i) {
        auto result = step(state, memory);
        if (result.status == StepStatus::fault) return result;
    }
    return {StepStatus::instruction_limit_reached, "instruction limit reached"};
}

} // namespace project360::cpu
