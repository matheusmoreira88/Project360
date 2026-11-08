#include "cpu/cpu_state.hpp"
#include "cpu/decoder.hpp"
#include "cpu/interpreter.hpp"
#include "memory/guest_memory.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::uint32_t d_form(std::uint8_t op, std::uint8_t rt, std::uint8_t ra, std::uint16_t imm) {
    return (static_cast<std::uint32_t>(op) << 26U) |
           (static_cast<std::uint32_t>(rt) << 21U) |
           (static_cast<std::uint32_t>(ra) << 16U) | imm;
}

std::uint32_t ds_form(std::uint8_t op, std::uint8_t rt, std::uint8_t ra,
                      std::int16_t displacement, std::uint8_t xo = 0) {
    const auto bits = static_cast<std::uint16_t>(displacement);
    const auto ds = static_cast<std::uint32_t>((bits >> 2U) & 0x3fffU);
    return (static_cast<std::uint32_t>(op) << 26U) |
           (static_cast<std::uint32_t>(rt) << 21U) |
           (static_cast<std::uint32_t>(ra) << 16U) | (ds << 2U) | (xo & 0x3U);
}

std::uint32_t add(std::uint8_t rd, std::uint8_t ra, std::uint8_t rb, bool record = false) {
    return (31U << 26U) | (static_cast<std::uint32_t>(rd) << 21U) |
           (static_cast<std::uint32_t>(ra) << 16U) | (static_cast<std::uint32_t>(rb) << 11U) |
           (266U << 1U) | (record ? 1U : 0U);
}

std::uint32_t fpr_unary(std::uint8_t fd, std::uint8_t fb, std::uint16_t xo,
                        bool record = false, std::uint8_t reserved_fra = 0) {
    return (63U << 26U) | (static_cast<std::uint32_t>(fd) << 21U) |
           (static_cast<std::uint32_t>(reserved_fra) << 16U) |
           (static_cast<std::uint32_t>(fb) << 11U) |
           (static_cast<std::uint32_t>(xo) << 1U) | (record ? 1U : 0U);
}

std::uint32_t add_xo(std::uint8_t rd, std::uint8_t ra, std::uint8_t rb, std::uint16_t xo,
                     bool overflow_enable = false, bool record = false) {
    return (31U << 26U) | (static_cast<std::uint32_t>(rd) << 21U) |
           (static_cast<std::uint32_t>(ra) << 16U) | (static_cast<std::uint32_t>(rb) << 11U) |
           (overflow_enable ? (1U << 10U) : 0U) | (static_cast<std::uint32_t>(xo) << 1U) |
           (record ? 1U : 0U);
}

std::uint32_t logical_x(std::uint8_t ra, std::uint8_t rs, std::uint8_t rb, std::uint16_t xo,
                        bool record = false) {
    return (31U << 26U) | (static_cast<std::uint32_t>(rs) << 21U) |
           (static_cast<std::uint32_t>(ra) << 16U) | (static_cast<std::uint32_t>(rb) << 11U) |
           (static_cast<std::uint32_t>(xo) << 1U) | (record ? 1U : 0U);
}

std::uint32_t mfcr_form(std::uint8_t rt, bool record = false,
                        std::uint8_t reserved_ra = 0, std::uint8_t reserved_rb = 0) {
    return (31U << 26U) | (static_cast<std::uint32_t>(rt) << 21U) |
           (static_cast<std::uint32_t>(reserved_ra) << 16U) |
           (static_cast<std::uint32_t>(reserved_rb) << 11U) | (19U << 1U) |
           (record ? 1U : 0U);
}

std::uint32_t mcrf_form(std::uint8_t bf, std::uint8_t bfa, std::uint32_t reserved = 0) {
    return (19U << 26U) | (static_cast<std::uint32_t>(bf) << 23U) |
           (static_cast<std::uint32_t>(bfa) << 18U) | reserved;
}

std::uint32_t cr_logical_form(std::uint8_t bt, std::uint8_t ba, std::uint8_t bb,
                              std::uint16_t xo, bool record = false) {
    return (19U << 26U) | (static_cast<std::uint32_t>(bt) << 21U) |
           (static_cast<std::uint32_t>(ba) << 16U) |
           (static_cast<std::uint32_t>(bb) << 11U) | (static_cast<std::uint32_t>(xo) << 1U) |
           (record ? 1U : 0U);
}

std::uint32_t special_register_form(bool to_special, std::uint8_t reg, std::uint16_t spr,
                                    bool record = false) {
    const auto encoded_spr = (static_cast<std::uint32_t>(spr & 0x1fU) << 16U) |
                             (static_cast<std::uint32_t>(spr & 0x3e0U) << 6U);
    const auto xo = to_special ? 467U : 339U;
    return (31U << 26U) | (static_cast<std::uint32_t>(reg) << 21U) | encoded_spr |
           (xo << 1U) | (record ? 1U : 0U);
}

std::uint32_t rfid_form(std::uint32_t reserved = 0) {
    return (19U << 26U) | (18U << 1U) | reserved;
}

std::uint32_t system_call_form(std::uint8_t level = 0U) {
    return (17U << 26U) | (level == 1U ? (1U << 5U) : 0U) | 2U;
}

std::uint32_t mtcrf_form(std::uint8_t fxm, std::uint8_t rs, bool record = false,
                         std::uint8_t reserved_instruction_bit11 = 0,
                         std::uint8_t reserved_instruction_bit20 = 0) {
    return (31U << 26U) | (static_cast<std::uint32_t>(rs) << 21U) |
           (static_cast<std::uint32_t>(reserved_instruction_bit11 & 1U) << 20U) |
           (static_cast<std::uint32_t>(fxm) << 12U) |
           (static_cast<std::uint32_t>(reserved_instruction_bit20 & 1U) << 11U) |
           (144U << 1U) | (record ? 1U : 0U);
}

std::uint32_t mcrxr_form(std::uint8_t bf, bool record = false,
                         std::uint8_t reserved_bf_low = 0, std::uint8_t reserved_ra = 0,
                         std::uint8_t reserved_rb = 0) {
    return (31U << 26U) | (static_cast<std::uint32_t>(bf) << 23U) |
           (static_cast<std::uint32_t>(reserved_bf_low & 0x3U) << 21U) |
           (static_cast<std::uint32_t>(reserved_ra) << 16U) |
           (static_cast<std::uint32_t>(reserved_rb) << 11U) | (512U << 1U) |
           (record ? 1U : 0U);
}

std::uint32_t indexed_memory(std::uint8_t rt, std::uint8_t ra, std::uint8_t rb,
                             std::uint16_t xo, bool record = false) {
    return (31U << 26U) | (static_cast<std::uint32_t>(rt) << 21U) |
           (static_cast<std::uint32_t>(ra) << 16U) | (static_cast<std::uint32_t>(rb) << 11U) |
           (static_cast<std::uint32_t>(xo) << 1U) | (record ? 1U : 0U);
}

std::uint32_t rotate_mask(std::uint8_t ra, std::uint8_t rs, std::uint8_t shift,
                          std::uint8_t begin, std::uint8_t end, bool record = false) {
    return (21U << 26U) | (static_cast<std::uint32_t>(rs) << 21U) |
           (static_cast<std::uint32_t>(ra) << 16U) | (static_cast<std::uint32_t>(shift) << 11U) |
           (static_cast<std::uint32_t>(begin) << 6U) | (static_cast<std::uint32_t>(end) << 1U) |
           (record ? 1U : 0U);
}

std::uint32_t rotate_word_form(std::uint8_t op, std::uint8_t ra, std::uint8_t rs,
                               std::uint8_t shift_or_rb, std::uint8_t begin,
                               std::uint8_t end, bool record = false) {
    return (static_cast<std::uint32_t>(op) << 26U) |
           (static_cast<std::uint32_t>(rs) << 21U) |
           (static_cast<std::uint32_t>(ra) << 16U) |
           (static_cast<std::uint32_t>(shift_or_rb) << 11U) |
           (static_cast<std::uint32_t>(begin) << 6U) |
           (static_cast<std::uint32_t>(end) << 1U) | (record ? 1U : 0U);
}

std::uint32_t rotate_doubleword_form(std::uint8_t ra, std::uint8_t rs, std::uint8_t shift,
                                     std::uint8_t mask, std::uint8_t xo,
                                     bool record = false) {
    return (30U << 26U) | (static_cast<std::uint32_t>(rs) << 21U) |
           (static_cast<std::uint32_t>(ra) << 16U) |
           (static_cast<std::uint32_t>(shift & 0x1fU) << 11U) |
           (static_cast<std::uint32_t>(mask & 0x3fU) << 5U) |
           (static_cast<std::uint32_t>(xo & 0x7U) << 2U) |
           (static_cast<std::uint32_t>(shift & 0x20U) >> 4U) | (record ? 1U : 0U);
}

std::uint32_t rotate_doubleword_variable_form(std::uint8_t ra, std::uint8_t rs,
                                              std::uint8_t rb, std::uint8_t mask,
                                              std::uint8_t xo, bool record = false) {
    return (30U << 26U) | (static_cast<std::uint32_t>(rs) << 21U) |
           (static_cast<std::uint32_t>(ra) << 16U) |
           (static_cast<std::uint32_t>(rb) << 11U) |
           (static_cast<std::uint32_t>(mask & 0x3fU) << 5U) |
           (static_cast<std::uint32_t>(xo & 0xfU) << 1U) | (record ? 1U : 0U);
}

std::uint32_t compare_immediate(std::uint8_t op, std::uint8_t bf, bool wide,
                                std::uint8_t ra, std::uint16_t immediate) {
    return (static_cast<std::uint32_t>(op) << 26U) |
           (static_cast<std::uint32_t>(bf) << 23U) | (wide ? (1U << 21U) : 0U) |
           (static_cast<std::uint32_t>(ra) << 16U) | immediate;
}

std::uint32_t compare_register(bool logical, std::uint8_t bf, bool wide,
                               std::uint8_t ra, std::uint8_t rb) {
    const auto xo = logical ? 32U : 0U;
    return (31U << 26U) | (static_cast<std::uint32_t>(bf) << 23U) |
           (wide ? (1U << 21U) : 0U) | (static_cast<std::uint32_t>(ra) << 16U) |
           (static_cast<std::uint32_t>(rb) << 11U) | (xo << 1U);
}

std::uint32_t branch_conditional(std::uint8_t bo, std::uint8_t bi, std::int16_t displacement,
                                bool link = false, bool absolute = false) {
    return (16U << 26U) | (static_cast<std::uint32_t>(bo) << 21U) |
           (static_cast<std::uint32_t>(bi) << 16U) |
           (static_cast<std::uint16_t>(displacement) & 0xfffcU) |
           (absolute ? 2U : 0U) | (link ? 1U : 0U);
}

std::uint32_t branch_register(std::uint8_t bo, std::uint8_t bi, std::uint16_t xo,
                              std::uint8_t bh = 0, bool link = false,
                              std::uint8_t reserved = 0) {
    return (19U << 26U) | (static_cast<std::uint32_t>(bo) << 21U) |
           (static_cast<std::uint32_t>(bi) << 16U) |
           (static_cast<std::uint32_t>(reserved & 7U) << 13U) |
           (static_cast<std::uint32_t>(bh & 3U) << 11U) |
           (static_cast<std::uint32_t>(xo) << 1U) | (link ? 1U : 0U);
}

void put_word(project360::memory::GuestMemory& memory, std::size_t address, std::uint32_t word) {
    check(memory.write_u32_be(address, word), "write test instruction");
}

void test_decode_and_execute_arithmetic() {
    using namespace project360;
    memory::GuestMemory mem(256);
    put_word(mem, 0, d_form(14, 3, 0, 7));
    put_word(mem, 4, d_form(14, 4, 3, static_cast<std::uint16_t>(-2)));
    put_word(mem, 8, add(5, 3, 4, true));
    cpu::CpuState state{};
    const auto result = cpu::Interpreter{}.run(state, mem, 3);
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "run reports limit after exactly three instructions");
    check(state.gpr[3] == 7 && state.gpr[4] == 5 && state.gpr[5] == 12,
          "addi/add execution matches expected GPR values");
    check((state.cr >> 28U) == 4U, "add record sets CR0.GT");
    check(state.pc == 12, "PC advances by four bytes per instruction");
}

void test_carry_and_overflow_arithmetic() {
    using namespace project360;
    memory::GuestMemory mem(128);
    put_word(mem, 0, add_xo(5, 1, 2, 10, false, true)); // addc. r5,r1,r2
    put_word(mem, 4, add_xo(6, 3, 4, 138, true, true)); // addeo. r6,r3,r4
    put_word(mem, 8, add_xo(7, 8, 9, 266, true)); // addo r7,r8,r9
    put_word(mem, 12, add_xo(10, 11, 12, 266, true)); // addo r10,r11,r12

    cpu::CpuState state{};
    state.gpr[1] = 0xffffffffffffffffULL;
    state.gpr[2] = 1;
    state.gpr[3] = 0x7fffffffffffffffULL;
    state.gpr[4] = 0;
    state.gpr[8] = 5;
    state.gpr[9] = 6;
    state.gpr[11] = 0x7fffffffffffffffULL;
    state.gpr[12] = 1;
    state.xer = 0x80000000U; // Sticky SO set; OV and CA clear.

    const cpu::Interpreter interpreter{};
    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[5] == 0,
          "addc produces the low 64-bit sum");
    check((state.xer & 0xe0000000U) == 0xa0000000U,
          "addc sets CA and preserves OV/SO when OE is clear");
    check((state.cr >> 28U) == 3U, "addc record updates CR0 and retains SO");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[6] == 0x8000000000000000ULL,
          "adde includes the incoming CA bit");
    check((state.xer & 0xe0000000U) == 0xc0000000U,
          "adde updates CA, OV and sticky SO when OE is set");
    check((state.cr >> 28U) == 9U, "record observes the updated SO bit");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[7] == 11,
          "non-overflowing addo returns the arithmetic result");
    check((state.xer & 0xe0000000U) == 0x80000000U,
          "non-overflowing OE form clears OV but keeps SO sticky");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[10] == 0x8000000000000000ULL,
          "overflowing addo wraps to the 64-bit result");
    check((state.xer & 0xe0000000U) == 0xc0000000U,
          "overflowing addo sets OV and SO");

    memory::GuestMemory carry_mem(16);
    put_word(carry_mem, 0, add_xo(3, 1, 2, 138)); // adde r3,r1,r2
    cpu::CpuState carry_state{};
    carry_state.gpr[1] = 0xffffffffffffffffULL;
    carry_state.gpr[2] = 0;
    carry_state.xer = 0x20000000U;
    const auto carry_result = interpreter.step(carry_state, carry_mem);
    check(carry_result.status == cpu::StepStatus::executed && carry_state.gpr[3] == 0 &&
              (carry_state.xer & 0x20000000U) != 0U,
          "adde reports carry out from the carry-in addition");
}

void test_addze_arithmetic() {
    using namespace project360;
    memory::GuestMemory mem(4);
    const cpu::Interpreter interpreter{};

    put_word(mem, 0, add_xo(1, 1, 0, 202)); // addze r1,r1
    cpu::CpuState carry_state{};
    carry_state.gpr[1] = 0xffffffffffffffffULL;
    carry_state.xer = 0xe0000000U;
    auto result = interpreter.step(carry_state, mem);
    check(result.status == cpu::StepStatus::executed && carry_state.gpr[1] == 0U &&
              carry_state.xer == 0xe0000000U,
          "addze uses XER.CA, supports RT=RA, and preserves OV/SO when OE is clear");

    put_word(mem, 0, add_xo(3, 4, 0, 202, true, true)); // addzeo. r3,r4
    cpu::CpuState overflow_state{};
    overflow_state.gpr[4] = 0x7fffffffffffffffULL;
    overflow_state.xer = 0x20000000U;
    result = interpreter.step(overflow_state, mem);
    check(result.status == cpu::StepStatus::executed &&
              overflow_state.gpr[3] == 0x8000000000000000ULL &&
              overflow_state.xer == 0xc0000000U && (overflow_state.cr >> 28U) == 9U,
          "addzeo. detects signed overflow, sets sticky SO and records the result in CR0");

    put_word(mem, 0, add_xo(5, 6, 0, 202, true)); // addzeo r5,r6
    cpu::CpuState no_overflow_state{};
    no_overflow_state.gpr[6] = 41;
    no_overflow_state.xer = 0xc0000000U;
    result = interpreter.step(no_overflow_state, mem);
    check(result.status == cpu::StepStatus::executed && no_overflow_state.gpr[5] == 41U &&
              no_overflow_state.xer == 0x80000000U,
          "non-overflowing addzeo clears OV while retaining sticky SO and updating CA");

    check(!cpu::decode(add_xo(3, 1, 1, 202)),
          "addze rejects a nonzero reserved RB field");
}

void test_negate_arithmetic() {
    using namespace project360;
    memory::GuestMemory mem(16);
    put_word(mem, 0, add_xo(3, 1, 0, 104, false, true)); // neg. r3,r1
    put_word(mem, 4, add_xo(2, 2, 0, 104)); // neg r2,r2
    put_word(mem, 8, add_xo(4, 5, 0, 104, true, true)); // nego. r4,r5
    put_word(mem, 12, add_xo(6, 7, 0, 104, true)); // nego r6,r7

    cpu::CpuState state{};
    state.gpr[1] = 5;
    state.gpr[2] = 0xfffffffffffffff9ULL; // -7
    state.gpr[5] = 0x8000000000000000ULL;
    state.gpr[7] = 42;
    state.xer = 0xa0000000U;
    const cpu::Interpreter interpreter{};

    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed &&
              state.gpr[3] == 0xfffffffffffffffbULL && (state.cr >> 28U) == 9U &&
              state.xer == 0xa0000000U,
          "neg. computes the two's-complement result and records CR0 with sticky SO");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[2] == 7U,
          "neg supports source/destination aliasing");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed &&
              state.gpr[4] == 0x8000000000000000ULL && state.xer == 0xe0000000U &&
              (state.cr >> 28U) == 9U,
          "nego. wraps signed minimum, sets OV/SO and records CR0");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[6] == 0xffffffffffffffd6ULL &&
              state.xer == 0xa0000000U,
          "non-overflowing nego clears OV and preserves sticky SO/CA");
    check(!cpu::decode(add_xo(3, 1, 2, 104)), "neg rejects a nonzero reserved RB field");
}

void test_carry_immediate_and_subtract_arithmetic() {
    using namespace project360;
    memory::GuestMemory mem(64);
    put_word(mem, 0, d_form(12, 5, 1, 1)); // addic r5,r1,1
    put_word(mem, 4, d_form(13, 6, 2, 0xffffU)); // addic. r6,r2,-1
    put_word(mem, 8, d_form(8, 7, 3, 10)); // subfic r7,r3,10
    put_word(mem, 12, d_form(8, 8, 4, 7)); // subfic r8,r4,7
    put_word(mem, 16, add_xo(9, 5, 6, 8, false, true)); // subfc. r9,r5,r6
    put_word(mem, 20, add_xo(10, 5, 6, 136)); // subfe r10,r5,r6
    put_word(mem, 24, add_xo(11, 12, 13, 40, true)); // subfo r11,r12,r13
    put_word(mem, 28, add_xo(14, 12, 13, 40)); // subf r14,r12,r13

    cpu::CpuState state{};
    state.gpr[1] = 0xffffffffffffffffULL;
    state.gpr[2] = 1;
    state.gpr[3] = 7;
    state.gpr[4] = 10;
    state.cr = 0x12345678U;
    const cpu::Interpreter interpreter{};

    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[5] == 0 &&
              (state.xer & 0x20000000U) != 0U && state.cr == 0x12345678U,
          "addic sets carry on unsigned wrap without updating CR");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[6] == 0 &&
              (state.xer & 0x20000000U) != 0U && (state.cr >> 28U) == 2U,
          "addic. reports unsigned carry and records CR0");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[7] == 3 &&
              (state.xer & 0x20000000U) != 0U,
          "subfic computes signed immediate minus RA and sets no-borrow CA");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[8] == 0xfffffffffffffffdULL &&
              (state.xer & 0x20000000U) == 0U,
          "subfic clears CA when subtraction borrows");

    state.gpr[5] = 3;
    state.gpr[6] = 5;
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[9] == 2 &&
              (state.xer & 0x20000000U) != 0U && (state.cr >> 28U) == 4U,
          "subfc subtracts RA from RB, sets no-borrow CA and records CR0");

    state.gpr[5] = 5;
    state.gpr[6] = 3;
    state.xer &= ~0x20000000U;
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[10] == 0xfffffffffffffffdULL &&
              (state.xer & 0x20000000U) == 0U,
          "subfe includes incoming CA as the no-borrow input");

    state.gpr[12] = 0xffffffffffffffffULL;
    state.gpr[13] = 0x7fffffffffffffffULL;
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[11] == 0x8000000000000000ULL &&
              (state.xer & 0xc0000000U) == 0xc0000000U,
          "subfo detects signed overflow and sets sticky SO");

    state.gpr[12] = 9;
    state.gpr[13] = 4;
    state.xer |= 0x40000000U;
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[14] == 0xfffffffffffffffbULL &&
              (state.xer & 0x40000000U) != 0U,
          "subf does not change CA or OV when OE is clear");
}

void test_logical_shift_and_rotate_instructions() {
    using namespace project360;
    memory::GuestMemory mem(128);
    put_word(mem, 0, logical_x(3, 1, 2, 28, true)); // and. r3,r1,r2
    put_word(mem, 4, logical_x(4, 1, 2, 60)); // andc r4,r1,r2
    put_word(mem, 8, logical_x(5, 1, 2, 284)); // eqv r5,r1,r2
    put_word(mem, 12, logical_x(6, 1, 2, 476)); // nand r6,r1,r2
    put_word(mem, 16, logical_x(7, 1, 2, 124)); // nor r7,r1,r2
    put_word(mem, 20, logical_x(8, 1, 2, 444)); // or r8,r1,r2
    put_word(mem, 24, logical_x(9, 1, 2, 412)); // orc r9,r1,r2
    put_word(mem, 28, logical_x(10, 1, 2, 316)); // xor r10,r1,r2
    put_word(mem, 32, logical_x(12, 11, 13, 24)); // slw r12,r11,r13
    put_word(mem, 36, logical_x(14, 11, 13, 536)); // srw r14,r11,r13
    put_word(mem, 40, logical_x(15, 11, 13, 792, true)); // sraw. r15,r11,r13
    put_word(mem, 44, rotate_mask(17, 16, 2, 0, 28, true)); // rlwinm. r17,r16,2,0,28
    put_word(mem, 48, rotate_mask(18, 19, 0, 28, 3)); // rlwinm r18,r19,0,28,3

    cpu::CpuState state{};
    state.gpr[1] = 0xf0;
    state.gpr[2] = 0x0f;
    state.gpr[11] = 0x80000003U;
    state.gpr[13] = 1;
    state.gpr[16] = 0x90003000U;
    state.gpr[19] = 0x9000300fU;
    const cpu::Interpreter interpreter{};
    const auto first = interpreter.step(state, mem);
    check(first.status == cpu::StepStatus::executed && state.gpr[3] == 0 &&
              (state.cr >> 28U) == 2U,
          "and record form writes the expected zero result and CR0");
    const auto result = interpreter.run(state, mem, 12);
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "logical, shift and rotate instruction stream executes");
    check(state.gpr[4] == 0xf0 && state.gpr[5] == 0xffffffffffffff00ULL &&
              state.gpr[6] == 0xffffffffffffffffULL && state.gpr[7] == 0xffffffffffffff00ULL &&
              state.gpr[8] == 0xff && state.gpr[9] == 0xfffffffffffffff0ULL && state.gpr[10] == 0xff,
          "and/or/xor complement variants operate across all 64 GPR bits");
    check(state.gpr[12] == 6 && state.gpr[14] == 0x40000001U,
          "slw and srw use the low word and zero-fill their results");
    check(state.gpr[15] == 0xffffffffc0000001ULL && (state.xer & 0x20000000U) != 0U,
          "sraw sign-extends the word and sets CA for discarded one bits of a negative value");
    check(state.gpr[17] == 0x4000c000U && (state.cr >> 28U) == 4U,
          "rlwinm rotates, applies an MSB-numbered mask and records CR0");
    check(state.gpr[18] == 0x9000000fU,
          "rlwinm supports a mask that wraps across the word boundary");

    memory::GuestMemory large_shift_mem(4);
    put_word(large_shift_mem, 0, logical_x(3, 1, 2, 792)); // sraw r3,r1,r2
    cpu::CpuState large_shift{};
    large_shift.gpr[1] = 0xffffffffU;
    large_shift.gpr[2] = 32;
    const auto large_result = interpreter.step(large_shift, large_shift_mem);
    check(large_result.status == cpu::StepStatus::executed &&
              large_shift.gpr[3] == 0xffffffffffffffffULL &&
              (large_shift.xer & 0x20000000U) != 0U,
          "sraw handles shift amounts of 32 or more without undefined shifts");
}

void test_doubleword_shift_instructions() {
    using namespace project360;
    memory::GuestMemory mem(64);
    put_word(mem, 0, logical_x(3, 1, 2, 27, true)); // sld. r3,r1,r2
    put_word(mem, 4, logical_x(4, 5, 6, 539, true)); // srd. r4,r5,r6
    put_word(mem, 8, logical_x(7, 1, 8, 27)); // sld r7,r1,r8
    put_word(mem, 12, logical_x(10, 5, 9, 539)); // srd r10,r5,r9
    put_word(mem, 16, logical_x(13, 11, 12, 794, true)); // srad. r13,r11,r12
    put_word(mem, 20, logical_x(16, 14, 15, 794)); // srad r16,r14,r15
    put_word(mem, 24, logical_x(19, 17, 18, 794)); // srad r19,r17,r18
    put_word(mem, 28, logical_x(22, 20, 21, 794)); // srad r22,r20,r21

    cpu::CpuState state{};
    state.gpr[1] = 0x4000000000000001ULL;
    state.gpr[2] = 1;
    state.gpr[5] = 0x8000000000000001ULL;
    state.gpr[6] = 1;
    state.gpr[8] = 64;
    state.gpr[9] = 127;
    state.gpr[11] = 0xfffffffffffffffdULL; // -3
    state.gpr[12] = 1;
    state.gpr[14] = 0xfffffffffffffffdULL; // -3
    state.gpr[15] = 64;
    state.gpr[17] = 1;
    state.gpr[18] = 127;
    state.gpr[20] = 0xffffffffffffffffULL; // -1
    state.gpr[21] = 0;
    state.xer = 0xe0000000U; // SO, OV, CA

    const cpu::Interpreter interpreter{};
    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[3] == 0x8000000000000002ULL &&
              (state.cr >> 28U) == 9U && state.xer == 0xe0000000U,
          "sld shifts 64-bit values, records CR0 and leaves XER unchanged");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[4] == 0x4000000000000000ULL &&
              (state.cr >> 28U) == 5U && state.xer == 0xe0000000U,
          "srd uses RB low seven bits, records CR0 and leaves XER unchanged");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[7] == 0,
          "sld returns zero for shift amounts 64 through 127");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[10] == 0,
          "srd returns zero for shift amounts 64 through 127");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[13] == 0xfffffffffffffffeULL &&
              (state.xer & 0x20000000U) != 0U && (state.cr >> 28U) == 9U,
          "srad sign-fills, sets CA for discarded one bits and records CR0");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[16] == 0xffffffffffffffffULL &&
              (state.xer & 0x20000000U) != 0U,
          "srad shifts by 64 or more to the sign fill and sets CA from the sign");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[19] == 0 &&
              (state.xer & 0x20000000U) == 0U,
          "srad clears CA when a positive source shifts without discarded negative bits");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[22] == 0xffffffffffffffffULL &&
              (state.xer & 0x20000000U) == 0U && state.pc == 32,
          "zero-count srad copies its source and clears CA");
}

void test_word_shift_immediate_instruction() {
    using namespace project360;
    memory::GuestMemory mem(32);
    put_word(mem, 0, logical_x(3, 1, 4, 824)); // srawi r3,r1,4
    put_word(mem, 4, logical_x(5, 4, 3, 824, true)); // srawi. r5,r4,3
    put_word(mem, 8, logical_x(7, 6, 1, 824)); // srawi r7,r6,1
    put_word(mem, 12, logical_x(9, 8, 31, 824, true)); // srawi. r9,r8,31
    put_word(mem, 16, logical_x(11, 10, 0, 824)); // srawi r11,r10,0

    cpu::CpuState state{};
    state.gpr[1] = 0x90003000U;
    state.gpr[4] = 0x80000007U;
    state.gpr[6] = 0x80000000U;
    state.gpr[8] = 0x80000001U;
    state.gpr[10] = 0xffffffffU;
    state.xer = 0xc0000000U; // SO and OV; CA starts clear

    const cpu::Interpreter interpreter{};
    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[3] == 0xfffffffff9000300ULL &&
              (state.xer & 0xe0000000U) == 0xc0000000U,
          "srawi sign-extends its low-word result and clears CA when no one bits are discarded");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[5] == 0xfffffffff0000000ULL &&
              (state.xer & 0xe0000000U) == 0xe0000000U && (state.cr >> 28U) == 9U,
          "srawi. sets CA for discarded one bits and records the signed result in CR0");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[7] == 0xffffffffc0000000ULL &&
              (state.xer & 0xe0000000U) == 0xc0000000U,
          "srawi clears CA when a negative source discards only zero bits");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[9] == 0xffffffffffffffffULL &&
              (state.xer & 0xe0000000U) == 0xe0000000U && (state.cr >> 28U) == 9U,
          "srawi handles the maximum immediate shift and records CR0");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[11] == 0xffffffffffffffffULL &&
              (state.xer & 0xe0000000U) == 0xc0000000U && state.pc == 20,
          "zero-count srawi sign-extends the source and clears CA");
}

void test_word_rotate_insert_and_variable_mask() {
    using namespace project360;
    memory::GuestMemory mem(16);
    put_word(mem, 0, rotate_word_form(20, 3, 4, 8, 8, 15, true)); // rlwimi. r3,r4,8,8,15
    put_word(mem, 4, rotate_word_form(23, 5, 4, 6, 28, 3, true)); // rlwnm. r5,r4,r6,28,3

    cpu::CpuState state{};
    state.gpr[3] = 0x12345678aaaaaaaaULL;
    state.gpr[4] = 0x12345678U;
    state.gpr[6] = 0x24U; // rlwnm uses only the low five bits: rotate by four
    const cpu::Interpreter interpreter{};

    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[3] == 0xaa56aaaaU &&
              (state.cr >> 28U) == 4U,
          "rlwimi inserts only selected rotated bits, clears the upper word, and records CR0");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[5] == 0x20000001U &&
              (state.cr >> 28U) == 4U && state.pc == 8,
          "rlwnm masks a wrapped word range and uses RB low five bits for rotation");

    const auto bad_word = cpu::decode(rotate_word_form(22, 1, 2, 3, 4, 5));
    check(!bad_word, "unassigned primary opcode between word rotate forms remains unsupported");
}

void test_doubleword_rotate_mask_instructions() {
    using namespace project360;
    memory::GuestMemory mem(32);
    put_word(mem, 0, rotate_doubleword_form(3, 1, 8, 16, 0)); // rldicl r3,r1,8,16
    put_word(mem, 4, rotate_doubleword_form(4, 1, 8, 39, 1)); // rldicr r4,r1,8,39
    put_word(mem, 8, rotate_doubleword_form(5, 1, 8, 0, 2)); // rldic r5,r1,8,0
    put_word(mem, 12, rotate_doubleword_form(6, 2, 48, 8, 3, true)); // rldimi. r6,r2,48,8
    put_word(mem, 16, rotate_doubleword_form(7, 1, 63, 0, 0)); // rldicl r7,r1,63,0
    put_word(mem, 20, rotate_doubleword_form(8, 1, 8, 60, 2)); // wrapped rldic mask

    cpu::CpuState state{};
    state.gpr[1] = 0x123456789abcdef0ULL;
    state.gpr[2] = 0xab;
    state.gpr[6] = 0x1122334455667788ULL;
    const cpu::Interpreter interpreter{};

    const std::uint64_t expected[] = {
        0x0000789abcdef012ULL,
        0x3456789abc000000ULL,
        0x3456789abcdef000ULL,
        0x11ab334455667788ULL,
        0x091a2b3c4d5e6f78ULL,
        0x3456789abcdef002ULL,
    };
    for (std::size_t index = 0; index < std::size(expected); ++index) {
        const auto result = interpreter.step(state, mem);
        const auto destination = static_cast<std::size_t>(index + 3U);
        if (result.status != cpu::StepStatus::executed || state.gpr[destination] != expected[index]) {
            std::cerr << "rotate-mask case " << index << ": got 0x" << std::hex
                      << state.gpr[destination] << ", expected 0x" << expected[index] << std::dec << '\n';
        }
        check(result.status == cpu::StepStatus::executed && state.gpr[destination] == expected[index],
              "64-bit rotate-mask instruction produces the expected result");
        if (index == 3U) {
            check((state.cr >> 28U) == 4U, "rldimi record form updates CR0 from the merged value");
        }
    }
    check(state.pc == 24, "all doubleword rotate-mask program instructions execute");

    const auto reserved = cpu::decode(rotate_doubleword_form(1, 2, 3, 4, 5));
    check(!reserved, "reserved MD-form rotate-mask subopcodes are diagnosed");
}

void test_variable_doubleword_rotate_mask_instructions() {
    using namespace project360;
    memory::GuestMemory mem(12);
    put_word(mem, 0, rotate_doubleword_variable_form(3, 1, 2, 8, 8, true)); // rldcl. r3,r1,r2,8
    put_word(mem, 4, rotate_doubleword_variable_form(4, 1, 2, 39, 9)); // rldcr r4,r1,r2,39

    cpu::CpuState state{};
    state.gpr[1] = 0x0123456789abcdefULL;
    state.gpr[2] = 0x41U; // low six bits select shift one
    const cpu::Interpreter interpreter{};

    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[3] == 0x00468acf13579bdeULL &&
              (state.cr >> 28U) == 4U,
          "rldcl rotates by RB low six bits, clears left bits and records CR0");

    state.gpr[2] = 0x7fU; // low six bits select shift 63
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[4] == 0x8091a2b3c4000000ULL &&
              state.pc == 8,
          "rldcr uses a wrapped six-bit register shift and clears right bits");

    const auto reserved = cpu::decode(rotate_doubleword_variable_form(1, 2, 3, 4, 10));
    check(!reserved, "reserved MDS-form rotate-mask subopcodes are diagnosed");
}

void test_leading_zero_instructions() {
    using namespace project360;
    memory::GuestMemory mem(32);
    put_word(mem, 0, logical_x(3, 1, 0, 26)); // cntlzw r3,r1
    put_word(mem, 4, logical_x(4, 2, 0, 26, true)); // cntlzw. r4,r2
    put_word(mem, 8, logical_x(5, 6, 0, 58)); // cntlzd r5,r6
    put_word(mem, 12, logical_x(7, 8, 0, 58, true)); // cntlzd. r7,r8
    put_word(mem, 16, logical_x(9, 10, 0, 58, true)); // cntlzd. r9,r10

    cpu::CpuState state{};
    state.gpr[1] = 0xffffffff00800000ULL; // high word is ignored by cntlzw
    state.gpr[2] = 0;
    state.gpr[6] = 0x0000800000000000ULL;
    state.gpr[8] = 0x8000000000000001ULL;
    state.gpr[10] = 0;
    state.xer = 0x80000000U; // SO is reflected by record forms

    const cpu::Interpreter interpreter{};
    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[3] == 8 &&
              state.cr == 0 && state.xer == 0x80000000U,
          "cntlzw examines only the low word and leaves CR/XER unchanged without Rc");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[4] == 32 &&
              (state.cr >> 28U) == 5U && state.xer == 0x80000000U,
          "cntlzw. returns 32 for zero and records GT plus SO in CR0");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[5] == 16 &&
              (state.cr >> 28U) == 5U,
          "cntlzd counts leading zeros across all 64 source bits");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[7] == 0 &&
              (state.cr >> 28U) == 3U,
          "cntlzd. returns zero when the high bit is set and records EQ plus SO");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[9] == 64 &&
              (state.cr >> 28U) == 5U && state.xer == 0x80000000U,
          "cntlzd. returns 64 for zero without changing XER");

    const auto bad_word_form = cpu::decode(logical_x(3, 1, 1, 26));
    const auto bad_doubleword_form = cpu::decode(logical_x(3, 1, 1, 58));
    check(!bad_word_form && !bad_doubleword_form,
          "cntlzw and cntlzd diagnose a nonzero reserved RB field");
}

void test_memory_and_endianness() {
    using namespace project360;
    memory::GuestMemory mem(512);
    put_word(mem, 0, d_form(14, 3, 0, 0x1234));
    put_word(mem, 4, d_form(36, 3, 0, 0x100));
    put_word(mem, 8, d_form(32, 4, 0, 0x100));
    cpu::CpuState state{};
    const auto result = cpu::Interpreter{}.run(state, mem, 3);
    std::uint32_t raw = 0;
    check(result.status == cpu::StepStatus::instruction_limit_reached, "memory program executes");
    check(mem.read_u32_be(0x100, raw) && raw == 0x1234U, "stw stores big-endian word");
    check(state.gpr[4] == 0x1234U, "lwz zero extends loaded word");
}

void test_byte_and_halfword_memory_instructions() {
    using namespace project360;
    memory::GuestMemory mem(512);
    put_word(mem, 0, d_form(14, 3, 0, 0x1234)); // addi r3,r0,0x1234
    put_word(mem, 4, d_form(38, 3, 0, 0x100)); // stb r3,0x100(r0)
    put_word(mem, 8, d_form(44, 3, 0, 0x102)); // sth r3,0x102(r0)
    put_word(mem, 12, d_form(34, 4, 0, 0x100)); // lbz r4,0x100(r0)
    put_word(mem, 16, d_form(40, 5, 0, 0x102)); // lhz r5,0x102(r0)
    put_word(mem, 20, d_form(42, 6, 0, 0x104)); // lha r6,0x104(r0)
    check(mem.write_u16_be(0x104, 0x8001U), "seed signed halfword data");

    cpu::CpuState state{};
    const auto result = cpu::Interpreter{}.run(state, mem, 6);
    std::uint8_t first_byte = 0;
    std::uint16_t stored_halfword = 0;
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "byte and halfword load/store program executes");
    check(mem.read_u8(0x100, first_byte) && first_byte == 0x34U,
          "stb stores the least significant byte");
    check(mem.read_u16_be(0x102, stored_halfword) && stored_halfword == 0x1234U,
          "sth stores a big-endian halfword");
    check(state.gpr[4] == 0x34U && state.gpr[5] == 0x1234U,
          "lbz and lhz zero-extend their loaded values");
    check(state.gpr[6] == 0xffffffffffff8001ULL,
          "lha sign-extends its loaded halfword to the 64-bit GPR");

    memory::GuestMemory fault_mem(16);
    put_word(fault_mem, 0, d_form(40, 3, 0, 1)); // lhz r3,1(r0), misaligned
    cpu::CpuState fault_state{};
    const auto fault = cpu::Interpreter{}.step(fault_state, fault_mem);
    check(fault.status == cpu::StepStatus::fault && fault_state.gpr[3] == 0 &&
              fault_state.pc == 0,
          "misaligned halfword load faults without changing architectural state");
}

void test_load_store_update_instructions() {
    using namespace project360;
    memory::GuestMemory mem(512);
    put_word(mem, 0, d_form(35, 2, 1, 1)); // lbzu r2,1(r1)
    put_word(mem, 4, d_form(41, 3, 1, 1)); // lhzu r3,1(r1)
    put_word(mem, 8, d_form(43, 4, 1, 2)); // lhau r4,2(r1)
    put_word(mem, 12, d_form(33, 5, 1, 4)); // lwzu r5,4(r1)
    put_word(mem, 16, d_form(39, 6, 1, 1)); // stbu r6,1(r1)
    put_word(mem, 20, d_form(45, 7, 1, 1)); // sthu r7,1(r1)
    put_word(mem, 24, d_form(37, 8, 1, 6)); // stwu r8,6(r1)
    check(mem.write_u8(0x101, 0x12U) && mem.write_u16_be(0x102, 0xbeefU) &&
              mem.write_u16_be(0x104, 0x8001U) && mem.write_u32_be(0x108, 0x12345678U),
          "seed update-form load data");

    cpu::CpuState state{};
    state.gpr[1] = 0x100;
    state.gpr[6] = 0x1234abcdU;
    state.gpr[7] = 0x9876fedcU;
    state.gpr[8] = 0xdeadbeefU;
    const auto result = cpu::Interpreter{}.run(state, mem, 7);
    std::uint8_t stored_byte = 0;
    std::uint16_t stored_half = 0;
    std::uint32_t stored_word = 0;
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "D-form load/store update program executes");
    check(state.gpr[2] == 0x12U && state.gpr[3] == 0xbeefU &&
              state.gpr[4] == 0xffffffffffff8001ULL && state.gpr[5] == 0x12345678U,
          "update loads preserve zero/sign extension semantics");
    check(state.gpr[1] == 0x110U,
          "each successful update form writes its effective address to RA");
    check(mem.read_u8(0x109, stored_byte) && stored_byte == 0xcdU &&
              mem.read_u16_be(0x10a, stored_half) && stored_half == 0xfedcU &&
              mem.read_u32_be(0x110, stored_word) && stored_word == 0xdeadbeefU,
          "update stores write data before committing the new base address");

    const auto load_alias = cpu::decode(d_form(35, 1, 1, 4));
    const auto zero_base = cpu::decode(d_form(35, 2, 0, 4));
    const auto store_zero_base = cpu::decode(d_form(39, 2, 0, 4));
    check(!load_alias && !zero_base && !store_zero_base,
          "invalid update forms reject RT=RA for loads and RA=0 for all updates");

    memory::GuestMemory fault_mem(32);
    put_word(fault_mem, 0, d_form(35, 2, 1, 8)); // lbzu r2,8(r1), out of range
    cpu::CpuState fault_state{};
    fault_state.gpr[1] = 24;
    fault_state.gpr[2] = 0x55;
    const auto fault = cpu::Interpreter{}.step(fault_state, fault_mem);
    check(fault.status == cpu::StepStatus::fault && fault_state.gpr[1] == 24 &&
              fault_state.gpr[2] == 0x55 && fault_state.pc == 0,
          "failed update load leaves base and target registers unchanged");

    memory::GuestMemory store_fault_mem(32);
    put_word(store_fault_mem, 0, d_form(37, 2, 1, 4)); // stwu r2,4(r1), out of range
    cpu::CpuState store_fault_state{};
    store_fault_state.gpr[1] = 28;
    store_fault_state.gpr[2] = 0xabcdef01U;
    const auto store_fault = cpu::Interpreter{}.step(store_fault_state, store_fault_mem);
    check(store_fault.status == cpu::StepStatus::fault && store_fault_state.gpr[1] == 28 &&
              store_fault_state.pc == 0,
          "failed update store leaves the base register unchanged");
}

void test_doubleword_memory_instructions() {
    using namespace project360;
    memory::GuestMemory mem(512);
    constexpr std::uint64_t first = 0x0123456789abcdefULL;
    constexpr std::uint64_t second = 0xfedcba9876543210ULL;
    constexpr std::uint64_t third = 0x80000000ffffffffULL;
    check(mem.write_u64_be(0x108, first) && mem.write_u64_be(0x118, second) &&
              mem.write_u64_be(0x130, third),
          "seed aligned doubleword data");

    put_word(mem, 0, d_form(14, 1, 0, 0x100)); // addi r1,r0,0x100
    put_word(mem, 4, ds_form(58, 3, 1, 8)); // ld r3,8(r1)
    put_word(mem, 8, ds_form(62, 3, 1, 16)); // std r3,16(r1)
    put_word(mem, 12, ds_form(58, 4, 1, 24, 1)); // ldu r4,24(r1)
    put_word(mem, 16, ds_form(62, 4, 1, 8, 1)); // stdu r4,8(r1)
    put_word(mem, 20, d_form(14, 2, 0, 16)); // addi r2,r0,16
    put_word(mem, 24, indexed_memory(5, 1, 2, 21)); // ldx r5,r1,r2
    put_word(mem, 28, indexed_memory(5, 1, 2, 149)); // stdx r5,r1,r2
    put_word(mem, 32, indexed_memory(6, 1, 2, 53)); // ldux r6,r1,r2
    put_word(mem, 36, indexed_memory(6, 1, 2, 181)); // stdux r6,r1,r2
    put_word(mem, 40, ds_form(58, 7, 1, -56)); // ld r7,-56(r1)
    put_word(mem, 44, ds_form(58, 8, 0, 0x108)); // ld r8,0x108(r0)

    cpu::CpuState state{};
    const auto result = cpu::Interpreter{}.run(state, mem, 12);
    std::uint64_t stored_first = 0;
    std::uint64_t stored_second = 0;
    std::uint64_t stored_third = 0;
    std::uint64_t stored_updated = 0;
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "DS-form and indexed doubleword program executes");
    check(state.gpr[3] == first && state.gpr[4] == second && state.gpr[5] == third &&
              state.gpr[6] == third && state.gpr[7] == first && state.gpr[8] == first,
          "ld/ldu/ldx/ldux preserve the full 64-bit value and EA rules");
    check(state.gpr[1] == 0x140U,
          "DS-form and indexed update loads/stores commit RA after success");
    check(mem.read_u64_be(0x110, stored_first) && stored_first == first &&
              mem.read_u64_be(0x120, stored_second) && stored_second == second &&
              mem.read_u64_be(0x130, stored_third) && stored_third == third &&
              mem.read_u64_be(0x140, stored_updated) && stored_updated == third,
          "std/stdu/stdx/stdux store big-endian doublewords");

    const auto negative_disp = cpu::decode(ds_form(58, 7, 1, -8));
    const auto bad_load_alias = cpu::decode(ds_form(58, 1, 1, 8, 1));
    const auto bad_load_zero_base = cpu::decode(ds_form(58, 2, 0, 8, 1));
    const auto bad_store_zero_base = cpu::decode(ds_form(62, 2, 0, 0, 1));
    const auto unsupported_lwa = cpu::decode(ds_form(58, 3, 1, 0, 2));
    const auto reserved_store_form = cpu::decode(ds_form(62, 3, 1, 0, 3));
    const auto bad_indexed_load_alias = cpu::decode(indexed_memory(1, 1, 2, 53));
    const auto bad_indexed_store_zero_base = cpu::decode(indexed_memory(2, 0, 2, 181));
    const auto bad_indexed_record = cpu::decode(indexed_memory(2, 1, 2, 21, true));
    check(negative_disp && negative_disp.instruction.immediate == -8,
          "DS-form displacement sign-extends and retains its byte scale");
    check(!bad_load_alias && !bad_load_zero_base && !bad_store_zero_base &&
              !unsupported_lwa && !reserved_store_form && !bad_indexed_load_alias &&
              !bad_indexed_store_zero_base && !bad_indexed_record,
          "invalid update operands, unsupported lwa and reserved DS/X-form bits fault at decode");

    memory::GuestMemory fault_mem(64);
    put_word(fault_mem, 0, ds_form(58, 3, 1, 0)); // ld r3,0(r1)
    cpu::CpuState fault_state{};
    fault_state.gpr[1] = 4;
    fault_state.gpr[3] = 0xabcdef0123456789ULL;
    const auto unaligned_load = cpu::Interpreter{}.step(fault_state, fault_mem);
    check(unaligned_load.status == cpu::StepStatus::fault && fault_state.gpr[3] == 0xabcdef0123456789ULL &&
              fault_state.pc == 0 && unaligned_load.fault_info &&
              unaligned_load.fault_info->kind == cpu::FaultKind::data_alignment &&
              unaligned_load.fault_info->pc == 0U &&
              unaligned_load.fault_info->instruction.has_value() &&
              unaligned_load.fault_info->effective_address == 4U,
          "unaligned ld reports structured address context without changing target or PC");

    memory::GuestMemory update_fault_mem(64);
    put_word(update_fault_mem, 0, ds_form(58, 4, 1, 8, 1)); // ldu r4,8(r1)
    cpu::CpuState update_fault_state{};
    update_fault_state.gpr[1] = 56;
    update_fault_state.gpr[4] = 0x55;
    const auto update_fault = cpu::Interpreter{}.step(update_fault_state, update_fault_mem);
    check(update_fault.status == cpu::StepStatus::fault && update_fault_state.gpr[1] == 56 &&
              update_fault_state.gpr[4] == 0x55 && update_fault_state.pc == 0 &&
              update_fault.fault_info &&
              update_fault.fault_info->kind == cpu::FaultKind::data_access &&
              update_fault.fault_info->effective_address == 64U,
          "out-of-range ldu reports the address and commits neither destination nor base");
}

void test_floating_point_load_store_transfers() {
    using namespace project360;
    memory::GuestMemory mem(512);
    check(mem.write_u32_be(0x100U, 0x3fc00000U) &&
              mem.write_u32_be(0x104U, 0x40100000U) &&
              mem.write_u64_be(0x120U, std::bit_cast<std::uint64_t>(6.5)),
          "seed IEEE binary32 and binary64 values"); // 1.5f, 2.25f and 6.5

    put_word(mem, 0, d_form(14, 1, 0, 0x100)); // addi r1,r0,0x100
    put_word(mem, 4, d_form(48, 2, 1, 0)); // lfs f2,0(r1)
    put_word(mem, 8, d_form(54, 2, 1, 8)); // stfd f2,8(r1)
    put_word(mem, 12, d_form(50, 3, 1, 8)); // lfd f3,8(r1)
    put_word(mem, 16, d_form(52, 3, 1, 16)); // stfs f3,16(r1)
    put_word(mem, 20, d_form(49, 4, 1, 4)); // lfsu f4,4(r1)
    put_word(mem, 24, d_form(53, 3, 1, 20)); // stfsu f3,20(r1)
    put_word(mem, 28, d_form(51, 5, 1, 8)); // lfdu f5,8(r1)
    put_word(mem, 32, d_form(55, 5, 1, 8)); // stfdu f5,8(r1)

    cpu::CpuState state{};
    const auto result = cpu::Interpreter{}.run(state, mem, 9);
    std::uint64_t stored_double = 0;
    std::uint64_t stored_updated_double = 0;
    std::uint32_t stored_single = 0;
    const auto one_and_a_half = std::bit_cast<std::uint64_t>(1.5);
    check(result.status == cpu::StepStatus::instruction_limit_reached &&
              state.fpr[2] == one_and_a_half && state.fpr[3] == one_and_a_half,
          "lfs widens binary32 to FPR and lfd preserves binary64 bits");
    check(mem.read_u64_be(0x108U, stored_double) && stored_double == one_and_a_half &&
              mem.read_u32_be(0x110U, stored_single) && stored_single == 0x3fc00000U,
          "stfd preserves the widened value and stfs converts it back to binary32");
    check(state.fpr[4] == std::bit_cast<std::uint64_t>(2.25) &&
              state.fpr[5] == std::bit_cast<std::uint64_t>(6.5) && state.gpr[1] == 0x128U &&
              mem.read_u64_be(0x128U, stored_updated_double) &&
              stored_updated_double == std::bit_cast<std::uint64_t>(6.5),
          "floating-point update loads/stores commit RA only after successful transfers");

    const auto bad_update = cpu::decode(d_form(49, 4, 0, 4));
    check(!bad_update, "floating-point update forms reject RA equal to zero");

    memory::GuestMemory fault_mem(64);
    put_word(fault_mem, 0, d_form(50, 5, 1, 0)); // lfd f5,0(r1)
    cpu::CpuState fault_state{};
    fault_state.gpr[1] = 4;
    fault_state.fpr[5] = 0x0123456789abcdefULL;
    const auto fault = cpu::Interpreter{}.step(fault_state, fault_mem);
    check(fault.status == cpu::StepStatus::fault && fault_state.fpr[5] == 0x0123456789abcdefULL &&
              fault_state.pc == 0 && fault.fault_info &&
              fault.fault_info->kind == cpu::FaultKind::data_alignment,
          "misaligned lfd reports a data-alignment fault without changing FPR or PC");

    memory::GuestMemory indexed_mem(512);
    check(indexed_mem.write_u32_be(0x120U, 0x40200000U) &&
              indexed_mem.write_u64_be(0x128U, std::bit_cast<std::uint64_t>(6.5)),
          "seed indexed floating-point transfer values");
    put_word(indexed_mem, 0, indexed_memory(1, 1, 2, 535)); // lfsx f1,r1,r2
    put_word(indexed_mem, 4, indexed_memory(1, 1, 3, 663)); // stfsx f1,r1,r3
    put_word(indexed_mem, 8, indexed_memory(2, 1, 4, 599)); // lfdx f2,r1,r4
    put_word(indexed_mem, 12, indexed_memory(2, 1, 5, 727)); // stfdx f2,r1,r5
    put_word(indexed_mem, 16, indexed_memory(6, 1, 2, 567)); // lfsux f6,r1,r2
    put_word(indexed_mem, 20, indexed_memory(6, 1, 6, 695)); // stfsux f6,r1,r6
    cpu::CpuState indexed_state{};
    indexed_state.gpr[1] = 0x100U;
    indexed_state.gpr[2] = 0x20U;
    indexed_state.gpr[3] = 0x24U;
    indexed_state.gpr[4] = 0x28U;
    indexed_state.gpr[5] = 0x30U;
    indexed_state.gpr[6] = 4U;
    const auto indexed_result = cpu::Interpreter{}.run(indexed_state, indexed_mem, 6);
    std::uint32_t indexed_single = 0;
    std::uint64_t indexed_double = 0;
    check(indexed_result.status == cpu::StepStatus::instruction_limit_reached &&
              indexed_state.fpr[1] == std::bit_cast<std::uint64_t>(2.5) &&
              indexed_state.fpr[2] == std::bit_cast<std::uint64_t>(6.5) &&
              indexed_state.fpr[6] == std::bit_cast<std::uint64_t>(2.5) &&
              indexed_state.gpr[1] == 0x124U,
          "indexed FPR loads and update-load address semantics execute correctly");
    check(indexed_mem.read_u32_be(0x124U, indexed_single) && indexed_single == 0x40200000U &&
              indexed_mem.read_u64_be(0x130U, indexed_double) &&
              indexed_double == std::bit_cast<std::uint64_t>(6.5),
          "indexed single/double stores preserve converted values and big-endian layout");

    memory::GuestMemory indexed_double_update_mem(512);
    (void)indexed_double_update_mem.write_u64_be(0x128U,
                                                  std::bit_cast<std::uint64_t>(6.5));
    put_word(indexed_double_update_mem, 0, indexed_memory(3, 1, 2, 631)); // lfdux f3,r1,r2
    put_word(indexed_double_update_mem, 4, indexed_memory(3, 1, 4, 759)); // stfdux f3,r1,r4
    cpu::CpuState indexed_double_update_state{};
    indexed_double_update_state.gpr[1] = 0x100U;
    indexed_double_update_state.gpr[2] = 0x28U;
    indexed_double_update_state.gpr[4] = 8U;
    const auto indexed_double_update_result =
        cpu::Interpreter{}.run(indexed_double_update_state, indexed_double_update_mem, 2);
    check(indexed_double_update_result.status == cpu::StepStatus::instruction_limit_reached &&
              indexed_double_update_state.fpr[3] == std::bit_cast<std::uint64_t>(6.5) &&
              indexed_double_update_state.gpr[1] == 0x130U &&
              indexed_double_update_mem.read_u64_be(0x130U, indexed_double) &&
              indexed_double == std::bit_cast<std::uint64_t>(6.5),
          "lfdux and stfdux update the base after successful doubleword transfers");

    check(!cpu::decode(indexed_memory(1, 0, 2, 567)) &&
              cpu::decode(indexed_memory(1, 1, 2, 567)) &&
              !cpu::decode(indexed_memory(1, 1, 2, 567, true)),
          "indexed floating update validates RA and the reserved record bit");
}

void test_floating_point_sign_and_move_instructions() {
    using namespace project360;
    memory::GuestMemory mem(32);
    put_word(mem, 0, fpr_unary(2, 1, 72, true)); // fmr. f2,f1
    put_word(mem, 4, fpr_unary(3, 1, 40)); // fneg f3,f1
    put_word(mem, 8, fpr_unary(4, 1, 264)); // fabs f4,f1
    put_word(mem, 12, fpr_unary(5, 3, 136)); // fnabs f5,f3

    cpu::CpuState state{};
    constexpr std::uint64_t kNegativeNanPayload = 0xfff0000012345678ULL;
    constexpr std::uint64_t kPositiveNanPayload = 0x7ff0000012345678ULL;
    state.fpr[1] = kNegativeNanPayload;
    state.fpscr = 0xa0000000U;
    state.cr = 0x12345678U;

    const auto result = cpu::Interpreter{}.run(state, mem, 4);
    const auto expected_cr1 = (0x12345678U & ~0x0f000000U) | 0x0a000000U;
    check(result.status == cpu::StepStatus::instruction_limit_reached &&
              state.fpr[2] == kNegativeNanPayload && state.fpr[3] == kPositiveNanPayload &&
              state.fpr[4] == kPositiveNanPayload && state.fpr[5] == kNegativeNanPayload,
          "fmr/fneg/fabs/fnabs preserve NaN payload bits and apply exact sign-bit semantics");
    check(state.cr == expected_cr1 && state.fpscr == 0xa0000000U,
          "record-form floating move copies FPSCR summary bits to CR1 without changing FPSCR");

    const auto reserved = cpu::decode(fpr_unary(2, 1, 72, false, 1));
    const auto unsupported = cpu::decode(fpr_unary(2, 1, 21));
    check(!reserved && !unsupported,
          "reserved unary FPR fields and unsupported arithmetic opcodes are diagnosed");
}

void test_indexed_memory_instructions() {
    using namespace project360;
    memory::GuestMemory mem(512);
    put_word(mem, 0, indexed_memory(3, 1, 2, 23)); // lwzx r3,r1,r2
    put_word(mem, 4, indexed_memory(4, 1, 5, 279)); // lhzx r4,r1,r5
    put_word(mem, 8, indexed_memory(6, 1, 7, 343)); // lhax r6,r1,r7
    put_word(mem, 12, indexed_memory(8, 0, 9, 87)); // lbzx r8,r0,r9
    put_word(mem, 16, indexed_memory(10, 1, 11, 215)); // stbx r10,r1,r11
    put_word(mem, 20, indexed_memory(12, 1, 13, 407)); // sthx r12,r1,r13
    put_word(mem, 24, indexed_memory(14, 1, 15, 151)); // stwx r14,r1,r15
    check(mem.write_u32_be(0x108, 0x12345678U) && mem.write_u16_be(0x104, 0xbeefU) &&
              mem.write_u16_be(0x106, 0x8001U) && mem.write_u8(0x101, 0x56U),
          "seed indexed load data");

    cpu::CpuState state{};
    state.gpr[1] = 0x100;
    state.gpr[2] = 8;
    state.gpr[5] = 4;
    state.gpr[7] = 6;
    state.gpr[9] = 0x101;
    state.gpr[10] = 0x1234abcdU;
    state.gpr[11] = 9;
    state.gpr[12] = 0x9876fedcU;
    state.gpr[13] = 10;
    state.gpr[14] = 0xdeadbeefU;
    state.gpr[15] = 16;
    const auto result = cpu::Interpreter{}.run(state, mem, 7);
    std::uint8_t stored_byte = 0;
    std::uint16_t stored_half = 0;
    std::uint32_t stored_word = 0;
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "indexed byte, halfword and word operations execute");
    check(state.gpr[3] == 0x12345678U && state.gpr[4] == 0xbeefU &&
              state.gpr[6] == 0xffffffffffff8001ULL && state.gpr[8] == 0x56U,
          "indexed loads calculate RA+RB, support RA=0 and extend values correctly");
    check(state.gpr[1] == 0x100U,
          "non-update indexed accesses leave the base register unchanged");
    check(mem.read_u8(0x109, stored_byte) && stored_byte == 0xcdU &&
              mem.read_u16_be(0x10a, stored_half) && stored_half == 0xfedcU &&
              mem.read_u32_be(0x110, stored_word) && stored_word == 0xdeadbeefU,
          "indexed stores use RA+RB and guest big-endian order");

    const auto reserved_record = cpu::decode(indexed_memory(3, 1, 2, 23, true));
    check(!reserved_record, "reserved record bit on indexed load/store is diagnosed");

    memory::GuestMemory fault_mem(32);
    put_word(fault_mem, 0, indexed_memory(3, 1, 2, 279)); // lhzx, misaligned
    cpu::CpuState fault_state{};
    fault_state.gpr[1] = 0;
    fault_state.gpr[2] = 1;
    const auto fault = cpu::Interpreter{}.step(fault_state, fault_mem);
    check(fault.status == cpu::StepStatus::fault && fault_state.gpr[3] == 0 &&
              fault_state.pc == 0,
          "misaligned indexed halfword load faults without modifying the target");
}

void test_indexed_update_memory_instructions() {
    using namespace project360;
    memory::GuestMemory mem(512);
    put_word(mem, 0, indexed_memory(3, 1, 20, 119)); // lbzux r3,r1,r20
    put_word(mem, 4, indexed_memory(4, 1, 21, 331)); // lhzux r4,r1,r21
    put_word(mem, 8, indexed_memory(5, 1, 22, 375)); // lhaux r5,r1,r22
    put_word(mem, 12, indexed_memory(6, 1, 23, 55)); // lwzux r6,r1,r23
    put_word(mem, 16, indexed_memory(7, 1, 24, 247)); // stbux r7,r1,r24
    put_word(mem, 20, indexed_memory(8, 1, 25, 439)); // sthux r8,r1,r25
    put_word(mem, 24, indexed_memory(9, 1, 26, 183)); // stwux r9,r1,r26
    check(mem.write_u8(0x101, 0x22U) && mem.write_u16_be(0x102, 0xbeefU) &&
              mem.write_u16_be(0x104, 0x8001U) && mem.write_u32_be(0x108, 0x13579bdfU),
          "seed indexed update loads");

    cpu::CpuState state{};
    state.gpr[1] = 0x100;
    state.gpr[20] = 1;
    state.gpr[21] = 1;
    state.gpr[22] = 2;
    state.gpr[23] = 4;
    state.gpr[24] = 8;
    state.gpr[25] = 2;
    state.gpr[26] = 6;
    state.gpr[7] = 0xabU;
    state.gpr[8] = 0xcdefU;
    state.gpr[9] = 0x12345678U;
    const auto result = cpu::Interpreter{}.run(state, mem, 7);
    std::uint8_t stored_byte = 0;
    std::uint16_t stored_half = 0;
    std::uint32_t stored_word = 0;
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          result.status == cpu::StepStatus::fault ? result.message.c_str()
                                                   : "indexed update load/store stream executes");
    check(state.gpr[3] == 0x22U && state.gpr[4] == 0xbeefU &&
              state.gpr[5] == 0xffffffffffff8001ULL && state.gpr[6] == 0x13579bdfU,
          "indexed update loads preserve zero/sign extension");
    check(state.gpr[1] == 0x118U,
          "indexed update loads and stores write each successful EA to RA");
    check(mem.read_u8(0x110, stored_byte) && stored_byte == 0xabU &&
              mem.read_u16_be(0x112, stored_half) && stored_half == 0xcdefU &&
              mem.read_u32_be(0x118, stored_word) && stored_word == 0x12345678U,
          "indexed update stores commit data at the effective address");

    const auto load_alias = cpu::decode(indexed_memory(1, 1, 2, 119));
    const auto zero_base_load = cpu::decode(indexed_memory(2, 0, 2, 119));
    const auto zero_base_store = cpu::decode(indexed_memory(2, 0, 2, 247));
    const auto reserved_record = cpu::decode(indexed_memory(2, 1, 2, 119, true));
    check(!load_alias && !zero_base_load && !zero_base_store && !reserved_record,
          "indexed update rejects RA=0, load RT=RA and reserved record bit");

    memory::GuestMemory load_fault_mem(32);
    put_word(load_fault_mem, 0, indexed_memory(3, 1, 2, 119));
    cpu::CpuState load_fault_state{};
    load_fault_state.gpr[1] = 16;
    load_fault_state.gpr[2] = 16;
    load_fault_state.gpr[3] = 0x55;
    const auto load_fault = cpu::Interpreter{}.step(load_fault_state, load_fault_mem);
    check(load_fault.status == cpu::StepStatus::fault && load_fault_state.gpr[1] == 16 &&
              load_fault_state.gpr[3] == 0x55 && load_fault_state.pc == 0,
          "failed indexed update load leaves RA and RT unchanged");

    memory::GuestMemory store_fault_mem(32);
    put_word(store_fault_mem, 0, indexed_memory(3, 1, 2, 183));
    cpu::CpuState store_fault_state{};
    store_fault_state.gpr[1] = 28;
    store_fault_state.gpr[2] = 4;
    store_fault_state.gpr[3] = 0x12345678U;
    const auto store_fault = cpu::Interpreter{}.step(store_fault_state, store_fault_mem);
    check(store_fault.status == cpu::StepStatus::fault && store_fault_state.gpr[1] == 28 &&
              store_fault_state.pc == 0,
          "failed indexed update store leaves RA unchanged");
}

void test_byte_reversed_indexed_memory_instructions() {
    using namespace project360;
    memory::GuestMemory mem(512);
    put_word(mem, 0, indexed_memory(3, 1, 2, 790)); // lhbrx r3,r1,r2
    put_word(mem, 4, indexed_memory(4, 1, 5, 534)); // lwbrx r4,r1,r5
    put_word(mem, 8, indexed_memory(6, 1, 7, 918)); // sthbrx r6,r1,r7
    put_word(mem, 12, indexed_memory(8, 1, 9, 662)); // stwbrx r8,r1,r9
    check(mem.write_u16_be(0x102, 0x3412U) && mem.write_u32_be(0x104, 0x78563412U),
          "seed byte-reversed indexed loads");

    cpu::CpuState state{};
    state.gpr[1] = 0x100;
    state.gpr[2] = 2;
    state.gpr[5] = 4;
    state.gpr[7] = 8;
    state.gpr[9] = 12;
    state.gpr[6] = 0x1234U;
    state.gpr[8] = 0x12345678U;
    const auto result = cpu::Interpreter{}.run(state, mem, 4);
    std::uint16_t stored_half = 0;
    std::uint32_t stored_word = 0;
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          result.status == cpu::StepStatus::fault ? result.message.c_str()
                                                   : "byte-reversed indexed stream executes");
    check(state.gpr[3] == 0x1234U && state.gpr[4] == 0x12345678U,
          "lhbrx and lwbrx reverse bytes while loading");
    check(mem.read_u16_be(0x108, stored_half) && stored_half == 0x3412U &&
              mem.read_u32_be(0x10c, stored_word) && stored_word == 0x78563412U,
          "sthbrx and stwbrx reverse bytes while storing");
    check(state.gpr[1] == 0x100U,
          "byte-reversed indexed forms do not update the base register");

    const auto reserved_record = cpu::decode(indexed_memory(3, 1, 2, 534, true));
    check(!reserved_record, "reserved record bit on byte-reversed access is diagnosed");

    memory::GuestMemory fault_mem(32);
    put_word(fault_mem, 0, indexed_memory(3, 1, 2, 534));
    cpu::CpuState fault_state{};
    fault_state.gpr[1] = 2;
    fault_state.gpr[2] = 1;
    fault_state.gpr[3] = 0x55U;
    const auto fault = cpu::Interpreter{}.step(fault_state, fault_mem);
    check(fault.status == cpu::StepStatus::fault && fault_state.gpr[3] == 0x55U &&
              fault_state.pc == 0,
          "misaligned lwbrx faults without modifying its destination");
}

void test_multiple_word_memory_instructions() {
    using namespace project360;
    memory::GuestMemory mem(512);
    put_word(mem, 0, d_form(46, 30, 1, 0xfffcU)); // lmw r30,-4(r1)
    put_word(mem, 4, d_form(47, 30, 1, 16)); // stmw r30,16(r1)
    put_word(mem, 8, d_form(46, 31, 0, 0x20)); // lmw r31,0(r0)
    check(mem.write_u32_be(0x100, 0x12345678U) &&
              mem.write_u32_be(0x104, 0x89abcdefU) &&
              mem.write_u32_be(0x20, 0xfeedfaceU),
          "seed multiple-word load data");

    cpu::CpuState state{};
    state.gpr[1] = 0x104;
    const auto result = cpu::Interpreter{}.run(state, mem, 3);
    std::uint32_t stored_first = 0;
    std::uint32_t stored_last = 0;
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          result.status == cpu::StepStatus::fault ? result.message.c_str()
                                                   : "lmw and stmw program executes");
    check(state.gpr[30] == 0x12345678U && state.gpr[31] == 0xfeedfaceU,
          "lmw loads registers through r31 using base plus signed displacement");
    check(mem.read_u32_be(0x114, stored_first) && stored_first == 0x12345678U &&
              mem.read_u32_be(0x118, stored_last) && stored_last == 0x89abcdefU,
          "stmw stores consecutive registers in guest big-endian order");

    const auto overlapping = cpu::decode(d_form(46, 30, 30, 0));
    const auto zero_alias = cpu::decode(d_form(46, 0, 0, 0));
    check(!overlapping && !zero_alias,
          "lmw diagnoses architecturally undefined base/destination overlap encodings");

    memory::GuestMemory fault_mem(32);
    put_word(fault_mem, 0, d_form(46, 30, 1, 0));
    cpu::CpuState load_fault_state{};
    load_fault_state.gpr[1] = 28;
    load_fault_state.gpr[30] = 0x11111111U;
    load_fault_state.gpr[31] = 0x22222222U;
    const auto load_fault = cpu::Interpreter{}.step(load_fault_state, fault_mem);
    check(load_fault.status == cpu::StepStatus::fault &&
              load_fault_state.gpr[30] == 0x11111111U &&
              load_fault_state.gpr[31] == 0x22222222U && load_fault_state.pc == 0,
          "failed lmw leaves its entire destination register range unchanged");

    put_word(fault_mem, 0, d_form(47, 30, 1, 0));
    cpu::CpuState store_fault_state{};
    store_fault_state.gpr[1] = 28;
    store_fault_state.gpr[30] = 0xaaaaaaaaU;
    store_fault_state.gpr[31] = 0xbbbbbbbbU;
    const auto store_fault = cpu::Interpreter{}.step(store_fault_state, fault_mem);
    std::uint32_t untouched = 1;
    check(store_fault.status == cpu::StepStatus::fault && fault_mem.read_u32_be(28, untouched) &&
              untouched == 0,
          "failed stmw validates the full range before writing any word");
}

void test_integer_divide_instructions() {
    using namespace project360;
    memory::GuestMemory mem(64);
    put_word(mem, 0, add_xo(3, 1, 2, 491, false, true)); // divw. r3,r1,r2
    put_word(mem, 4, add_xo(4, 4, 5, 459)); // divwu r4,r4,r5 (RT=RA)
    put_word(mem, 8, add_xo(6, 6, 7, 489)); // divd r6,r6,r7 (RT=RA)
    put_word(mem, 12, add_xo(8, 9, 10, 457, false, true)); // divdu. r8,r9,r10
    put_word(mem, 16, add_xo(11, 12, 13, 491, true, true)); // divwo. invalid signed overflow
    put_word(mem, 20, add_xo(14, 15, 16, 459, true)); // divwuo zero divisor
    put_word(mem, 24, add_xo(17, 18, 19, 489, true)); // divdo valid clears OV
    put_word(mem, 28, add_xo(20, 21, 22, 457)); // divdu zero divisor, OE clear

    cpu::CpuState state{};
    state.gpr[1] = 0xfffffffffffffff9ULL; // -7
    state.gpr[2] = 2;
    state.gpr[4] = 0x00000002ffffffffULL;
    state.gpr[5] = 2;
    state.gpr[6] = 0xfffffffffffffff9ULL; // -7
    state.gpr[7] = 2;
    state.gpr[9] = 0xffffffffffffffffULL;
    state.gpr[10] = 10;
    state.gpr[12] = 0x80000000U;
    state.gpr[13] = 0xffffffffU;
    state.gpr[15] = 77;
    state.gpr[16] = 0;
    state.gpr[18] = 84;
    state.gpr[19] = 2;
    state.gpr[21] = 10;
    state.gpr[22] = 0;
    state.xer = 0xa0000000U;

    const cpu::Interpreter interpreter{};
    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[3] == 0xfffffffdU &&
              (state.cr >> 28U) == 9U,
          "divw writes the low-word quotient and records its signed result with XER.SO");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[4] == 0x7fffffffU,
          "divwu zero-extends the low-word quotient and supports RT=RA");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed &&
              state.gpr[6] == 0xfffffffffffffffdULL,
          "divd uses signed 64-bit operands and truncates the quotient toward zero");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed &&
              state.gpr[8] == 0x1999999999999999ULL && (state.cr >> 28U) == 5U,
          "divdu computes unsigned 64-bit quotient and records CR0");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[11] == 0U &&
              state.xer == 0xe0000000U && (state.cr >> 28U) == 3U,
          "invalid divwo. uses deterministic zero, sets OV/SO and updates CR0");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[14] == 0U &&
              state.xer == 0xe0000000U,
          "invalid divwuo sets OV while preserving sticky SO and CA");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[17] == 42U &&
              state.xer == 0xa0000000U,
          "valid divdo clears OV while preserving sticky SO and CA");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[20] == 0U &&
              state.xer == 0xa0000000U,
          "invalid division without OE preserves XER");

    memory::GuestMemory record_mem(4);
    put_word(record_mem, 0, add_xo(3, 1, 2, 459, false, true)); // divwu. r3,r1,r2
    cpu::CpuState record_state{};
    record_state.gpr[1] = 0x80000000U;
    record_state.gpr[2] = 1U;
    const auto record_result = cpu::Interpreter{}.step(record_state, record_mem);
    check(record_result.status == cpu::StepStatus::executed &&
              record_state.gpr[3] == 0x80000000U && (record_state.cr >> 28U) == 8U,
          "divwu record compares the low-word result as signed for CR0");

    put_word(record_mem, 0, add_xo(3, 1, 2, 489, true, true)); // divdo. r3,r1,r2
    cpu::CpuState overflow_state{};
    overflow_state.gpr[1] = 0x8000000000000000ULL;
    overflow_state.gpr[2] = 0xffffffffffffffffULL;
    overflow_state.xer = 0xa0000000U;
    const auto overflow_result = cpu::Interpreter{}.step(overflow_state, record_mem);
    check(overflow_result.status == cpu::StepStatus::executed && overflow_state.gpr[3] == 0U &&
              overflow_state.xer == 0xe0000000U && (overflow_state.cr >> 28U) == 3U,
          "divdo. handles signed 64-bit overflow without host division UB");
}

void test_integer_multiply_instructions() {
    using namespace project360;
    memory::GuestMemory mem(128);
    put_word(mem, 0, d_form(7, 3, 1, 0xfffeU)); // mulli r3,r1,-2
    put_word(mem, 4, d_form(7, 2, 0, 3)); // mulli r2,r0,3
    put_word(mem, 8, add_xo(4, 5, 6, 235, true, true)); // mullwo. r4,r5,r6
    put_word(mem, 12, add_xo(7, 8, 9, 235, true)); // mullwo r7,r8,r9
    put_word(mem, 16, add_xo(10, 11, 12, 233, true, true)); // mulldo. r10,r11,r12
    put_word(mem, 20, add_xo(13, 14, 15, 233, true)); // mulldo r13,r14,r15

    cpu::CpuState state{};
    state.gpr[0] = 5;
    state.gpr[1] = 0x0000000200000001ULL;
    state.gpr[5] = 0x80000000U;
    state.gpr[6] = 2;
    state.gpr[8] = 7;
    state.gpr[9] = 6;
    state.gpr[11] = 0x7fffffffffffffffULL;
    state.gpr[12] = 2;
    state.gpr[14] = 0x8000000000000000ULL;
    state.gpr[15] = 0xfffffffffffffffeULL;
    state.cr = 0x12345678U;
    state.xer = 0x20000000U;

    const cpu::Interpreter interpreter{};
    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed &&
              state.gpr[3] == 0xfffffffbfffffffeULL && state.cr == 0x12345678U &&
              state.xer == 0x20000000U,
          "mulli uses 64-bit operands, signed immediate and preserves CR/XER");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[2] == 15,
          "mulli reads GPR0 as an ordinary arithmetic source register");

    result = interpreter.run(state, mem, 4);
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          result.status == cpu::StepStatus::fault ? result.message.c_str()
                                                   : "mullw and mulld program executes");
    check(state.gpr[4] == 0U,
          "mullw returns the defined low word and records its signed value");
    check(state.gpr[7] == 42,
          "mullw with OE clears OV when the signed word product fits");
    check(state.gpr[10] == 0xfffffffffffffffeULL,
          "mulld returns the low doubleword of a signed 64-bit product");
    check(state.gpr[13] == 0,
          "mulld wraps the low doubleword for an overflowing negative product");
    check((state.xer & 0xe0000000U) == 0xe0000000U && (state.cr >> 28U) == 9U,
          "multiply overflow updates OV/SO and record updates CR0");
}

void test_integer_high_word_multiply_instructions() {
    using namespace project360;
    memory::GuestMemory mem(8);
    put_word(mem, 0, add_xo(3, 1, 2, 75, false, true)); // mulhw. r3,r1,r2
    put_word(mem, 4, add_xo(4, 4, 5, 11, false, true)); // mulhwu. r4,r4,r5

    cpu::CpuState state{};
    state.gpr[1] = 0xfffffffffffffff9ULL; // -7
    state.gpr[2] = 2;
    state.gpr[4] = 0xffffffffU;
    state.gpr[5] = 0xffffffffU;
    state.xer = 0x80000000U;
    const cpu::Interpreter interpreter{};

    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[3] == 0xffffffffU &&
              (state.cr >> 28U) == 9U && state.xer == 0x80000000U,
          "mulhw. returns the signed high word and records CR0 without changing XER");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[4] == 0xfffffffeU &&
              (state.cr >> 28U) == 9U && state.xer == 0x80000000U,
          "mulhwu. returns the unsigned high word, handles RT=RA and records signed CR0");
}

void test_integer_high_doubleword_multiply_instructions() {
    using namespace project360;
    memory::GuestMemory mem(8);
    put_word(mem, 0, add_xo(3, 1, 2, 73, false, true)); // mulhd. r3,r1,r2
    put_word(mem, 4, add_xo(4, 4, 5, 9, false, true)); // mulhdu. r4,r4,r5

    cpu::CpuState state{};
    state.gpr[1] = 0xfffffffffffffffeULL; // -2
    state.gpr[2] = 3;
    state.gpr[4] = 0xffffffffffffffffULL;
    state.gpr[5] = 0xffffffffffffffffULL;
    state.xer = 0x80000000U;
    const cpu::Interpreter interpreter{};

    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed &&
              state.gpr[3] == 0xffffffffffffffffULL && (state.cr >> 28U) == 9U &&
              state.xer == 0x80000000U,
          "mulhd. produces the signed high half and preserves XER");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed &&
              state.gpr[4] == 0xfffffffffffffffeULL && (state.cr >> 28U) == 9U &&
              state.xer == 0x80000000U,
          "mulhdu. produces the unsigned high half with RT=RA and records CR0");
}

void test_sign_extension_instructions() {
    using namespace project360;
    memory::GuestMemory mem(128);
    put_word(mem, 0, logical_x(3, 1, 0, 954)); // extsb r3,r1
    put_word(mem, 4, logical_x(4, 2, 0, 922, true)); // extsh. r4,r2
    put_word(mem, 8, logical_x(5, 6, 0, 986)); // extsw r5,r6
    put_word(mem, 12, logical_x(7, 8, 0, 986, true)); // extsw. r7,r8

    cpu::CpuState state{};
    state.gpr[1] = 0x123456789abcde80ULL;
    state.gpr[2] = 0x123456789abc8001ULL;
    state.gpr[6] = 0x1234567880000001ULL;
    state.gpr[8] = 0x0000000180000000ULL;
    state.xer = 0xa0000000U;
    state.cr = 0x12345678U;
    const auto result = cpu::Interpreter{}.run(state, mem, 4);
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          result.status == cpu::StepStatus::fault ? result.message.c_str()
                                                   : "sign-extension instruction stream executes");
    check(state.gpr[3] == 0xffffffffffffff80ULL &&
              state.gpr[4] == 0xffffffffffff8001ULL &&
              state.gpr[5] == 0xffffffff80000001ULL &&
              state.gpr[7] == 0xffffffff80000000ULL,
          "extsb/extsh/extsw sign-extend their low 8/16/32-bit values");
    check((state.cr >> 28U) == 9U && state.xer == 0xa0000000U,
          "record form updates CR0 while sign-extension forms preserve XER");

    const auto reserved_half = cpu::decode(logical_x(3, 1, 2, 922));
    const auto reserved_word = cpu::decode(logical_x(3, 1, 2, 986));
    check(!reserved_half && !reserved_word,
          "sign-extension instructions diagnose nonzero reserved fields");

    memory::GuestMemory alias_mem(64);
    put_word(alias_mem, 0, logical_x(1, 1, 0, 954, true)); // extsb. r1,r1
    cpu::CpuState alias_state{};
    alias_state.gpr[1] = 0x123456789abcde80ULL;
    const auto alias_result = cpu::Interpreter{}.step(alias_state, alias_mem);
    check(alias_result.status == cpu::StepStatus::executed &&
              alias_state.gpr[1] == 0xffffffffffffff80ULL,
          "sign-extension instruction safely supports source/destination aliasing");
}

void test_immediate_logic_instructions() {
    using namespace project360;
    memory::GuestMemory mem(128);
    put_word(mem, 0, d_form(15, 3, 0, 1)); // addis r3, r0, 1
    put_word(mem, 4, d_form(24, 3, 4, 0x1234)); // ori r4, r3, 0x1234
    put_word(mem, 8, d_form(25, 4, 5, 2)); // oris r5, r4, 2
    cpu::CpuState state{};
    const auto result = cpu::Interpreter{}.run(state, mem, 3);
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "immediate logic program executes");
    check(state.gpr[3] == 0x10000U, "addis shifts the signed immediate by 16 bits");
    check(state.gpr[4] == 0x11234U, "ori uses RS, RA and an unsigned immediate");
    check(state.gpr[5] == 0x31234U, "oris shifts the unsigned immediate by 16 bits");
}

void test_xor_and_immediate_logic_instructions() {
    using namespace project360;
    memory::GuestMemory mem(32);
    put_word(mem, 0, d_form(26, 1, 2, 0x00ffU)); // xori r2,r1,0x00ff
    put_word(mem, 4, d_form(27, 1, 3, 0x0f0fU)); // xoris r3,r1,0x0f0f
    put_word(mem, 8, d_form(28, 1, 4, 0xf0f0U)); // andi. r4,r1,0xf0f0
    put_word(mem, 12, d_form(29, 1, 5, 0x00ffU)); // andis. r5,r1,0x00ff

    cpu::CpuState state{};
    state.gpr[1] = 0x123456789abcdef0ULL;
    state.cr = 0xa1234567U;
    state.xer = 0x80000000U;
    const cpu::Interpreter interpreter{};

    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed &&
              state.gpr[2] == (0x123456789abcdef0ULL ^ 0x00ffULL) &&
              state.cr == 0xa1234567U && state.xer == 0x80000000U,
          "xori XORs the unsigned immediate without changing CR or XER");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed &&
              state.gpr[3] == (0x123456789abcdef0ULL ^ 0x0f0f0000ULL) &&
              state.cr == 0xa1234567U && state.xer == 0x80000000U,
          "xoris places its immediate in bits 16 through 31 without changing flags");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[4] == 0xd0f0U &&
              (state.cr >> 28U) == 5U && (state.cr & 0x0fffffffU) == 0x01234567U &&
              state.xer == 0x80000000U,
          "andi. masks the low word and writes CR0 with the existing SO bit");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[5] == 0x00bc0000U &&
              (state.cr >> 28U) == 5U && state.xer == 0x80000000U,
          "andis. masks the shifted immediate and records its result");
}

void test_branch_and_faults() {
    using namespace project360;
    memory::GuestMemory mem(64);
    put_word(mem, 0, d_form(14, 3, 0, 1));
    put_word(mem, 4, (18U << 26U) | (8U & 0x03fffffcU) | 1U); // b +8, link
    put_word(mem, 8, d_form(14, 3, 0, 99)); // skipped
    put_word(mem, 12, d_form(14, 4, 0, 2));
    cpu::CpuState state{};
    const auto result = cpu::Interpreter{}.run(state, mem, 3);
    check(result.status == cpu::StepStatus::instruction_limit_reached, "linked branch program executes");
    check(state.gpr[3] == 1 && state.gpr[4] == 2, "relative branch skips one instruction");
    check(state.pc == 16 && state.lr == 8, "branch sets PC and LR correctly");

    auto bad = cpu::decode(0U);
    check(!bad && !bad.error.empty(), "unknown opcode is diagnosed");
    state.pc = 64;
    const auto fetch = cpu::Interpreter{}.step(state, mem);
    check(fetch.status == cpu::StepStatus::fault && fetch.fault_info.has_value() &&
              fetch.fault_info->kind == cpu::FaultKind::instruction_access &&
              fetch.fault_info->pc == 64U && !fetch.fault_info->instruction.has_value(),
          "out-of-range instruction fetch reports structured access-fault context");

    state.pc = 2;
    const auto unaligned_fetch = cpu::Interpreter{}.step(state, mem);
    check(unaligned_fetch.status == cpu::StepStatus::fault && unaligned_fetch.fault_info &&
              unaligned_fetch.fault_info->kind == cpu::FaultKind::instruction_alignment &&
              unaligned_fetch.fault_info->pc == 2U,
          "unaligned instruction fetch reports an alignment fault");

    put_word(mem, 16, 0U);
    state.pc = 16;
    const auto illegal = cpu::Interpreter{}.step(state, mem);
    check(illegal.status == cpu::StepStatus::exception_taken && illegal.fault_info &&
              illegal.fault_info->kind == cpu::FaultKind::illegal_instruction &&
              illegal.fault_info->pc == 16U && illegal.fault_info->instruction == 0U &&
              illegal.guest_exception && illegal.guest_exception->vector == 0x700U &&
              state.srr0 == 16U && state.srr1 == (1ULL << 19U) && state.pc == 0x700U,
          "invalid instruction records SRR state and transfers to the program vector");

    check(!cpu::decode(branch_conditional(0x15U, 0, 4)),
          "reserved conditional branch option is diagnosed");
}

void test_program_exception_and_rfid_return() {
    using namespace project360;
    memory::GuestMemory mem(0x800);
    constexpr std::uint64_t msr_sf = 1ULL << 63U;
    constexpr std::uint64_t msr_pr = 1ULL << 14U;
    constexpr std::uint64_t program_illegal_bit = 1ULL << 19U;
    constexpr std::uint64_t program_privileged_bit = 1ULL << 18U;

    put_word(mem, 0, 0U); // Illegal instruction enters the program exception vector.
    put_word(mem, 4, d_form(14, 8, 0, 8));
    put_word(mem, 0x700, d_form(14, 3, 0, 7));
    put_word(mem, 0x704, special_register_form(false, 4, 26)); // Read SRR0.
    put_word(mem, 0x708, special_register_form(false, 5, 27)); // Read SRR1.
    put_word(mem, 0x70c, special_register_form(true, 6, 26)); // Set return PC.
    put_word(mem, 0x710, special_register_form(true, 7, 27)); // Set return MSR.
    put_word(mem, 0x714, rfid_form());

    cpu::CpuState state{};
    state.msr = msr_sf;
    state.gpr[6] = 4;
    state.gpr[7] = msr_sf;
    const auto result = cpu::Interpreter{}.run(state, mem, 8);
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "program exception handler and rfid return execute within the instruction budget");
    check(state.gpr[3] == 7 && state.gpr[4] == 0 &&
              state.gpr[5] == (msr_sf | program_illegal_bit),
          "exception handler observes fault PC and saved illegal-instruction syndrome");
    check(state.srr0 == 4 && state.srr1 == msr_sf && state.msr == msr_sf &&
              state.gpr[8] == 8 && state.pc == 8,
          "rfid restores the selected SRR0/SRR1 state and resumes the guest program");

    check(cpu::decode(rfid_form()).instruction.opcode ==
              cpu::Opcode::return_from_interrupt_doubleword &&
              !cpu::decode(rfid_form(1U << 6U)) && !cpu::decode(rfid_form(1U)),
          "rfid decodes only with reserved fields and Rc clear");

    memory::GuestMemory privileged_mem(4);
    put_word(privileged_mem, 0, rfid_form());
    cpu::CpuState privileged_state{};
    privileged_state.msr = msr_sf | msr_pr;
    const auto privileged = cpu::Interpreter{}.step(privileged_state, privileged_mem);
    check(privileged.status == cpu::StepStatus::exception_taken && privileged.fault_info &&
              privileged.fault_info->kind == cpu::FaultKind::privileged_instruction &&
              privileged_state.srr0 == 0 && privileged_state.srr1 ==
                  (msr_sf | msr_pr | program_privileged_bit) && privileged_state.pc == 0x700,
          "problem-state rfid causes a privileged-instruction program exception");

    put_word(privileged_mem, 0, special_register_form(false, 3, 26));
    cpu::CpuState privileged_spr_state{};
    privileged_spr_state.msr = msr_sf | msr_pr;
    const auto privileged_spr = cpu::Interpreter{}.step(privileged_spr_state, privileged_mem);
    check(privileged_spr.status == cpu::StepStatus::exception_taken &&
              privileged_spr.fault_info &&
              privileged_spr.fault_info->kind == cpu::FaultKind::privileged_instruction &&
              privileged_spr_state.pc == 0x700,
          "problem-state access to SRR0 enters the privileged-instruction exception vector");

    put_word(privileged_mem, 0, rfid_form());
    cpu::CpuState sf32_state{};
    sf32_state.srr0 = 0x12345678;
    const auto sf32_return = cpu::Interpreter{}.step(sf32_state, privileged_mem);
    check(sf32_return.status == cpu::StepStatus::executed && sf32_state.pc == 0x12345678,
          "rfid uses the low 32-bit return address when restored MSR.SF is clear");
}

void test_system_call_exception_and_return() {
    using namespace project360;
    memory::GuestMemory mem(0x1000U);
    put_word(mem, 0, d_form(14, 2, 0, 1)); // addi r2,r0,1
    put_word(mem, 4, system_call_form()); // sc 0
    put_word(mem, 8, d_form(14, 4, 0, 7)); // addi r4,r0,7
    put_word(mem, 0xc00U, d_form(14, 3, 0, 42)); // kernel handler body
    put_word(mem, 0xc04U, rfid_form()); // return to SRR0

    cpu::CpuState state{};
    const auto msr_bit = [](unsigned bit) { return std::uint64_t{1} << (63U - bit); };
    state.msr = msr_bit(0U) | msr_bit(3U) | msr_bit(11U) | msr_bit(49U) |
                msr_bit(51U) | msr_bit(58U) | msr_bit(59U) | msr_bit(62U);
    const auto original_msr = state.msr;
    const auto decoded = cpu::decode(system_call_form());
    check(decoded && decoded.instruction.opcode == cpu::Opcode::system_call,
          "SC-form sc 0 decodes as a system call");
    const auto hypervisor_call = cpu::decode(system_call_form(1U));
    check(hypervisor_call && hypervisor_call.instruction.level == 1U &&
              !cpu::decode((17U << 26U) | 2U | (1U << 31U)),
          "sc LEV=1 is decoded distinctly and reserved SC-form fields are rejected");
    memory::GuestMemory hypervisor_mem(4U);
    put_word(hypervisor_mem, 0, system_call_form(1U));
    cpu::CpuState hypervisor_state{};
    const auto unsupported_hypercall = cpu::Interpreter{}.step(hypervisor_state, hypervisor_mem);
    check(unsupported_hypercall.status == cpu::StepStatus::fault &&
              unsupported_hypercall.fault_info &&
              unsupported_hypercall.fault_info->kind == cpu::FaultKind::unsupported_instruction &&
              hypervisor_state.pc == 0U && hypervisor_state.srr0 == 0U,
          "unsupported hypervisor call reports a host diagnostic without fake guest success");

    const cpu::Interpreter interpreter{};
    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[2] == 1U,
          "guest executes a setup instruction before sc");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::exception_taken && !result.fault_info &&
              result.guest_exception &&
              result.guest_exception->kind == cpu::GuestExceptionKind::system_call &&
              result.guest_exception->vector == 0xc00U && state.pc == 0xc00U &&
              state.srr0 == 8U && state.srr1 == original_msr &&
              state.msr == (msr_bit(0U) | msr_bit(3U) | msr_bit(51U)),
          "sc saves the next PC and OEA MSR state, then vectors to 0xC00");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[3] == 42U &&
              state.pc == 0xc04U,
          "system-call handler executes guest code");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.pc == 8U &&
              state.msr == (original_msr | msr_bit(48U)),
          "rfid restores the pre-system-call privilege state and resumes after sc");
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[4] == 7U &&
              state.pc == 12U,
          "guest resumes at the instruction immediately following sc");
}

void test_cpu_fetch_from_mapped_executable_page() {
    using namespace project360;
    memory::GuestMemory mem(2U * static_cast<std::size_t>(memory::GuestMemory::page_size));
    put_word(mem, 0x1000, d_form(14, 3, 0, 5));
    check(mem.map_page(0x4000, 0x1000, memory::GuestPageAccess::execute),
          "guest code virtual page maps with execute permission");
    cpu::CpuState state{};
    state.pc = 0x4000;
    auto result = cpu::Interpreter{}.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[3] == 5 && state.pc == 0x4004,
          "PowerPC interpreter fetches and executes code through virtual address translation");

    check(mem.protect_page(0x4000, memory::GuestPageAccess::read),
          "test changes executable page to a non-executable permission");
    state.pc = 0x4000;
    result = cpu::Interpreter{}.step(state, mem);
    check(result.status == cpu::StepStatus::fault && result.fault_info &&
              result.fault_info->kind == cpu::FaultKind::instruction_access &&
              state.gpr[3] == 5 && state.pc == 0x4000,
          "CPU rejects instruction fetch when its mapped page lacks execute permission");
}

void test_compare_immediates() {
    using namespace project360;
    memory::GuestMemory mem(128);
    put_word(mem, 0, compare_immediate(11, 1, false, 3, 0xffffU)); // cmpi cr1,0,r3,-1
    put_word(mem, 4, compare_immediate(10, 2, true, 3, 0xffffU)); // cmpldi cr2,r3,65535
    put_word(mem, 8, compare_immediate(11, 0, true, 3, 0xffffU)); // cmpdi cr0,r3,-1
    put_word(mem, 12, compare_immediate(10, 4, false, 3, 0xffffU)); // cmplwi cr4,r3,65535
    cpu::CpuState state{};
    state.gpr[3] = 0xfffffffffffffffeULL;
    state.cr = 0x01234567U;
    state.xer = 0x80000000U;
    const auto result = cpu::Interpreter{}.run(state, mem, 4);
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "cmpi and cmpli program executes");
    check(((state.cr >> 24U) & 0xfU) == 0x9U,
          "word cmpi compares signed low 32 bits and copies XER.SO");
    check(((state.cr >> 20U) & 0xfU) == 0x5U,
          "doubleword cmpli compares unsigned 64-bit operands");
    check(((state.cr >> 28U) & 0xfU) == 0x9U,
          "doubleword cmpi compares signed 64-bit operands");
    check(((state.cr >> 12U) & 0xfU) == 0x5U,
          "word cmpli compares only low 32 bits");
    check((state.cr & 0x00000fffU) == 0x567U,
          "compare updates only its selected CR field");

    const auto reserved = cpu::decode(compare_immediate(11, 0, false, 3, 0xffffU) | (1U << 22U));
    check(!reserved, "reserved compare immediate bit is diagnosed");
}

void test_compare_registers() {
    using namespace project360;
    memory::GuestMemory mem(128);
    put_word(mem, 0, compare_register(false, 5, true, 1, 2)); // cmp cr5,1,2
    put_word(mem, 4, compare_register(false, 6, false, 1, 2)); // cmpw cr6,1,2
    put_word(mem, 8, compare_register(true, 7, true, 1, 2)); // cmpl cr7,1,2
    put_word(mem, 12, compare_register(true, 4, false, 1, 2)); // cmplw cr4,1,2
    put_word(mem, 16, compare_register(false, 0, true, 3, 4)); // cmp cr0,3,4

    cpu::CpuState state{};
    state.gpr[1] = 0xffffffff80000002ULL;
    state.gpr[2] = 0x000000007fffffffULL;
    state.gpr[3] = 44;
    state.gpr[4] = 44;
    state.cr = 0xaaaaaaaaU;
    state.xer = 0x80000000U;

    const auto result = cpu::Interpreter{}.run(state, mem, 5);
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "cmp and cmpl register program executes");
    check(((state.cr >> 8U) & 0xfU) == 0x9U,
          "64-bit signed cmp compares the complete GPR values");
    check(((state.cr >> 4U) & 0xfU) == 0x9U,
          "32-bit signed cmp interprets the low words as signed values");
    check((state.cr & 0xfU) == 0x5U,
          "64-bit unsigned cmpl compares the complete GPR values");
    check(((state.cr >> 12U) & 0xfU) == 0x5U,
          "32-bit unsigned cmpl compares the low words");
    check(((state.cr >> 28U) & 0xfU) == 0x3U,
          "cmp sets EQ and copies XER.SO");
    check((state.cr & 0x0fff0000U) == 0x0aaa0000U,
          "register compares preserve every unselected CR field");

    const auto reserved_bit = cpu::decode(compare_register(false, 0, true, 1, 2) | (1U << 22U));
    const auto reserved_record = cpu::decode(compare_register(true, 0, false, 1, 2) | 1U);
    check(!reserved_bit && !reserved_record,
          "reserved cmp/cmpl bit forms are diagnosed");
}

void test_condition_register_transfers() {
    using namespace project360;
    memory::GuestMemory mem(12);
    put_word(mem, 0, mfcr_form(5)); // mfcr r5
    put_word(mem, 4, mtcrf_form(0xa0U, 6)); // mtcrf 0xa0,r6: CR0 and CR2
    put_word(mem, 8, mtcrf_form(0, 7)); // empty FXM preserves CR

    cpu::CpuState state{};
    state.cr = 0x11223344U;
    state.xer = 0xe0000000U;
    state.gpr[5] = 0xffffffffffffffffULL;
    state.gpr[6] = 0xa9876543U;
    const cpu::Interpreter interpreter{};

    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.gpr[5] == 0x11223344U &&
              state.cr == 0x11223344U && state.xer == 0xe0000000U,
          "mfcr copies the complete CR without changing CR or XER");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.cr == 0xa1823344U &&
              state.xer == 0xe0000000U,
          "mtcrf copies selected GPR nibbles to CR0 and CR2, preserving other fields");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.cr == 0xa1823344U && state.pc == 12,
          "mtcrf with an empty FXM leaves every CR field unchanged");

    check(!cpu::decode(mfcr_form(1, true)) && !cpu::decode(mfcr_form(1, false, 1)) &&
              !cpu::decode(mfcr_form(1, false, 0, 1)),
          "mfcr record and reserved-field encodings are rejected");
    check(!cpu::decode(mtcrf_form(0xffU, 1, true)) &&
              !cpu::decode(mtcrf_form(0xffU, 1, false, 1)) &&
              !cpu::decode(mtcrf_form(0xffU, 1, false, 0, 1)),
          "mtcrf record and reserved-field encodings are rejected");
}

void test_move_condition_register_field() {
    using namespace project360;
    memory::GuestMemory mem(12);
    put_word(mem, 0, mcrf_form(0, 4)); // mcrf cr0,cr4
    put_word(mem, 4, mcrf_form(7, 0)); // mcrf cr7,cr0
    put_word(mem, 8, mcrf_form(7, 7)); // self-copy

    cpu::CpuState state{};
    state.cr = 0x12345678U;
    state.xer = 0xe1234567U;
    const cpu::Interpreter interpreter{};

    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.cr == 0x52345678U &&
              state.xer == 0xe1234567U,
          "mcrf copies the selected source field and preserves other CR fields and XER");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.cr == 0x52345675U,
          "mcrf can copy a changed field to CR7");

    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.cr == 0x52345675U && state.pc == 12,
          "mcrf self-copy leaves the condition register unchanged");

    check(!cpu::decode(mcrf_form(2, 3, 1U << 21U)) &&
              !cpu::decode(mcrf_form(2, 3, 1U << 16U)) &&
              !cpu::decode(mcrf_form(2, 3, 1U << 11U)) &&
              !cpu::decode(mcrf_form(2, 3, 1U)),
          "mcrf reserved fields and record bit are rejected");
}

void test_condition_register_logical_operations() {
    using namespace project360;
    constexpr std::uint8_t target_bit = 9;
    constexpr std::uint8_t source_a_bit = 2;
    constexpr std::uint8_t source_b_bit = 17;
    constexpr std::uint32_t target_mask = 1U << (31U - target_bit);
    constexpr std::uint32_t source_a_mask = 1U << (31U - source_a_bit);
    constexpr std::uint32_t source_b_mask = 1U << (31U - source_b_bit);
    constexpr std::uint32_t base_cr = 0x5aa55aa5U & ~target_mask;
    constexpr std::array<std::pair<std::uint16_t, cpu::CrLogicalOperation>, 8> operations{{
        {257U, cpu::CrLogicalOperation::and_},
        {129U, cpu::CrLogicalOperation::and_complement},
        {289U, cpu::CrLogicalOperation::equivalent},
        {225U, cpu::CrLogicalOperation::nand},
        {33U, cpu::CrLogicalOperation::nor},
        {449U, cpu::CrLogicalOperation::or_},
        {417U, cpu::CrLogicalOperation::or_complement},
        {193U, cpu::CrLogicalOperation::xor_},
    }};
    memory::GuestMemory mem(4);
    const cpu::Interpreter interpreter{};

    for (const auto& [xo, operation] : operations) {
        for (unsigned inputs = 0; inputs < 4U; ++inputs) {
            const bool a = (inputs & 1U) != 0U;
            const bool b = (inputs & 2U) != 0U;
            auto cr = base_cr & ~(source_a_mask | source_b_mask);
            if (a) cr |= source_a_mask;
            if (b) cr |= source_b_mask;
            if ((inputs & 2U) != 0U) cr |= target_mask;
            put_word(mem, 0, cr_logical_form(target_bit, source_a_bit, source_b_bit, xo));

            cpu::CpuState state{};
            state.cr = cr;
            state.xer = 0xd2345678U;
            const auto result = interpreter.step(state, mem);
            const bool expected = [&] {
                switch (operation) {
                case cpu::CrLogicalOperation::and_: return a && b;
                case cpu::CrLogicalOperation::and_complement: return a && !b;
                case cpu::CrLogicalOperation::equivalent: return a == b;
                case cpu::CrLogicalOperation::nand: return !(a && b);
                case cpu::CrLogicalOperation::nor: return !(a || b);
                case cpu::CrLogicalOperation::or_: return a || b;
                case cpu::CrLogicalOperation::or_complement: return a || !b;
                case cpu::CrLogicalOperation::xor_: return a != b;
                }
                return false;
            }();
            const auto expected_cr = expected ? (cr | target_mask) : (cr & ~target_mask);
            check(result.status == cpu::StepStatus::executed && state.cr == expected_cr &&
                      state.xer == 0xd2345678U,
                  "CR logical operations implement their full truth tables and preserve other state");
        }
    }

    check(!cpu::decode(cr_logical_form(target_bit, source_a_bit, source_b_bit, 257U, true)),
          "CR logical record-bit encodings are rejected");

    put_word(mem, 0, cr_logical_form(source_a_bit, source_a_bit, source_b_bit, 257U));
    cpu::CpuState alias_state{};
    alias_state.cr = source_a_mask | source_b_mask;
    const auto alias_result = interpreter.step(alias_state, mem);
    check(alias_result.status == cpu::StepStatus::executed &&
              (alias_state.cr & source_a_mask) != 0U,
          "CR logical operations read source bits before writing an aliased target");
}

void test_special_register_transfers() {
    using namespace project360;
    memory::GuestMemory mem(24);
    put_word(mem, 0, special_register_form(false, 3, 1)); // mfspr r3,XER
    put_word(mem, 4, special_register_form(false, 4, 8)); // mfspr r4,LR
    put_word(mem, 8, special_register_form(false, 5, 9)); // mfspr r5,CTR
    put_word(mem, 12, special_register_form(true, 6, 1)); // mtspr XER,r6
    put_word(mem, 16, special_register_form(true, 7, 8)); // mtspr LR,r7
    put_word(mem, 20, special_register_form(true, 8, 9)); // mtspr CTR,r8

    cpu::CpuState state{};
    state.xer = 0x81234567U;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.cr = 0x5a5aa5a5U;
    state.gpr[6] = 0x12345678abcdef12ULL;
    state.gpr[7] = 0x0123456789abcdefULL;
    state.gpr[8] = 0xfedcba9876543210ULL;
    const cpu::Interpreter interpreter{};

    for (unsigned i = 0; i < 6U; ++i) {
        const auto result = interpreter.step(state, mem);
        check(result.status == cpu::StepStatus::executed,
              "supported mfspr/mtspr instruction executes");
    }
    check(state.gpr[3] == 0x81234567U && state.gpr[4] == 0x1122334455667788ULL &&
              state.gpr[5] == 0x8877665544332211ULL,
          "mfspr reads XER, LR and CTR into the selected GPRs");
    check(state.xer == 0xabcdef12U, "mtspr XER write truncates to XER width");
    check(state.lr == 0x0123456789abcdefULL, "mtspr writes LR");
    check(state.ctr == 0xfedcba9876543210ULL, "mtspr writes CTR");
    check(state.cr == 0x5a5aa5a5U && state.pc == 24,
          "special-register transfers preserve CR and advance PC");

    check(!cpu::decode(special_register_form(false, 3, 2)) &&
              !cpu::decode(special_register_form(true, 3, 2)) &&
              !cpu::decode(special_register_form(false, 3, 1, true)) &&
              !cpu::decode(special_register_form(true, 3, 9, true)),
          "unsupported SPR numbers and reserved record encodings are rejected");
}

void test_move_condition_register_from_xer() {
    using namespace project360;
    memory::GuestMemory mem(8);
    put_word(mem, 0, mcrxr_form(4)); // mcrxr cr4
    put_word(mem, 4, mcrxr_form(0)); // mcrxr cr0

    cpu::CpuState state{};
    state.cr = 0x12345678U;
    state.xer = 0xd2345678U; // SO, OV, CA and XER bit 3 transfer as nibble D.
    const cpu::Interpreter interpreter{};

    auto result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.cr == 0x1234d678U &&
              state.xer == 0x02345678U,
          "mcrxr moves the complete XER top nibble to the selected CR field and clears it");

    state.xer = 0xeabcdef0U;
    result = interpreter.step(state, mem);
    check(result.status == cpu::StepStatus::executed && state.cr == 0xe234d678U &&
              state.xer == 0x0abcdef0U && state.pc == 8,
          "mcrxr can update CR0 and preserves the lower XER bits");

    check(!cpu::decode(mcrxr_form(3, true)) && !cpu::decode(mcrxr_form(3, false, 1)) &&
              !cpu::decode(mcrxr_form(3, false, 0, 1)) &&
              !cpu::decode(mcrxr_form(3, false, 0, 0, 1)),
          "mcrxr record and reserved-field encodings are rejected");
}

void test_conditional_branches() {
    using namespace project360;
    memory::GuestMemory mem(128);
    put_word(mem, 0, compare_immediate(11, 0, true, 3, 7)); // cmpdi cr0,r3,7
    put_word(mem, 4, branch_conditional(12, 2, 8, true)); // beq+ +8
    put_word(mem, 8, d_form(14, 4, 0, 99)); // skipped
    put_word(mem, 12, d_form(14, 5, 0, 5));
    cpu::CpuState state{};
    state.gpr[3] = 7;
    const auto result = cpu::Interpreter{}.run(state, mem, 3);
    check(result.status == cpu::StepStatus::instruction_limit_reached,
          "CR conditional branch program executes");
    check(state.gpr[4] == 0 && state.gpr[5] == 5,
          "bc tests the selected CR bit and branches when it matches BO");
    check(state.pc == 16 && state.lr == 8, "taken bcl updates NIA and LR");

    memory::GuestMemory ctr_mem(64);
    put_word(ctr_mem, 0, branch_conditional(16, 0, 8)); // bdnz +8
    cpu::CpuState ctr_state{};
    ctr_state.ctr = 2;
    const auto taken = cpu::Interpreter{}.step(ctr_state, ctr_mem);
    check(taken.status == cpu::StepStatus::executed && ctr_state.pc == 8 && ctr_state.ctr == 1,
          "bc decrements CTR and branches while the result is nonzero");

    ctr_state.pc = 0;
    ctr_state.ctr = 1;
    const auto not_taken = cpu::Interpreter{}.step(ctr_state, ctr_mem);
    check(not_taken.status == cpu::StepStatus::executed && ctr_state.pc == 4 && ctr_state.ctr == 0,
          "bc falls through when decremented CTR reaches zero");

    memory::GuestMemory false_mem(64);
    put_word(false_mem, 0, branch_conditional(4, 2, 8, true)); // bne+ with CR0.EQ set
    cpu::CpuState false_state{};
    false_state.cr = 0x20000000U;
    const auto false_result = cpu::Interpreter{}.step(false_state, false_mem);
    check(false_result.status == cpu::StepStatus::executed && false_state.pc == 4 &&
              false_state.lr == 4,
          "bc falls through on a false CR condition and still records LK");

    memory::GuestMemory zero_mem(64);
    put_word(zero_mem, 0, branch_conditional(18, 0, 8)); // bdz +8
    cpu::CpuState zero_state{};
    zero_state.ctr = 1;
    const auto zero_result = cpu::Interpreter{}.step(zero_state, zero_mem);
    check(zero_result.status == cpu::StepStatus::executed && zero_state.pc == 8 &&
              zero_state.ctr == 0,
          "bc zero-count option branches after CTR decrements to zero");
}

void test_absolute_and_register_branches() {
    using namespace project360;
    memory::GuestMemory absolute_mem(64);
    put_word(absolute_mem, 0, (18U << 26U) | 12U | 2U | 1U); // ba 12, link
    put_word(absolute_mem, 12, d_form(14, 3, 0, 9));
    cpu::CpuState absolute_state{};
    auto result = cpu::Interpreter{}.step(absolute_state, absolute_mem);
    check(result.status == cpu::StepStatus::executed && absolute_state.pc == 12 &&
              absolute_state.lr == 4,
          "absolute I-form branch targets its signed address and links");
    result = cpu::Interpreter{}.step(absolute_state, absolute_mem);
    check(result.status == cpu::StepStatus::executed && absolute_state.gpr[3] == 9,
          "absolute branch reaches and executes the addressed instruction");

    memory::GuestMemory conditional_mem(64);
    put_word(conditional_mem, 0, branch_conditional(12, 2, 16, true, true)); // beqa 16, link
    put_word(conditional_mem, 16, d_form(14, 4, 0, 7));
    cpu::CpuState conditional_state{};
    conditional_state.cr = 0x20000000U;
    result = cpu::Interpreter{}.step(conditional_state, conditional_mem);
    check(result.status == cpu::StepStatus::executed && conditional_state.pc == 16 &&
              conditional_state.lr == 4,
          "absolute B-form branch uses CR/BO and writes LR");

    memory::GuestMemory lr_mem(64);
    put_word(lr_mem, 0, branch_register(16, 0, 16, 0, true)); // bdnzlr to old LR
    cpu::CpuState lr_state{};
    lr_state.lr = 15;
    lr_state.ctr = 2;
    result = cpu::Interpreter{}.step(lr_state, lr_mem);
    check(result.status == cpu::StepStatus::executed && lr_state.pc == 12 && lr_state.lr == 4 &&
              lr_state.ctr == 1,
          "bclr aligns and uses the old LR target before linking and decrementing CTR");

    memory::GuestMemory ctr_mem(64);
    put_word(ctr_mem, 0, branch_register(20, 0, 528, 0, true)); // bcctrl, no CTR decrement
    cpu::CpuState ctr_state{};
    ctr_state.ctr = 15;
    result = cpu::Interpreter{}.step(ctr_state, ctr_mem);
    check(result.status == cpu::StepStatus::executed && ctr_state.pc == 12 &&
              ctr_state.ctr == 15 && ctr_state.lr == 4,
          "bcctr branches to aligned CTR without decrementing its target");

    const auto bad_count = cpu::decode(branch_register(16, 0, 528));
    const auto bad_reserved = cpu::decode(branch_register(20, 0, 16, 0, false, 1));
    const auto bad_hint = cpu::decode(branch_register(20, 0, 16, 1));
    check(!bad_count && !bad_reserved && !bad_hint,
          "invalid bcctr BO and reserved branch-register fields are diagnosed");
}

void test_memory_bounds_and_alignment() {
    project360::memory::GuestMemory mem(8);
    std::uint32_t value = 0;
    check(!mem.read_u32_be(1, value), "unaligned word read rejected");
    check(!mem.read_u32_be(8, value), "out-of-range word read rejected");
    check(mem.write_u32_be(4, 0xaabbccddU), "valid word write succeeds");
    check(mem.read_u32_be(4, value) && value == 0xaabbccddU, "word round trip succeeds");
    std::uint16_t halfword = 0;
    check(mem.write_u16_be(2, 0x1234U) && mem.read_u16_be(2, halfword) && halfword == 0x1234U,
          "valid aligned halfword round trip succeeds");
    check(!mem.read_u16_be(1, halfword) && !mem.write_u16_be(1, 0x5678U),
          "unaligned halfword access rejected");
    check(!mem.read_u16_be(8, halfword) && !mem.write_u16_be(8, 0x5678U),
          "out-of-range halfword access rejected");
    std::uint64_t doubleword = 0;
    check(mem.write_u64_be(0, 0x0123456789abcdefULL) &&
              mem.read_u64_be(0, doubleword) && doubleword == 0x0123456789abcdefULL,
          "aligned 64-bit big-endian round trip succeeds");
    check(!mem.read_u64_be(4, doubleword) && !mem.write_u64_be(4, 0xffffffffffffffffULL),
          "unaligned doubleword access rejected");
    check(!mem.read_u64_be(8, doubleword) && !mem.write_u64_be(8, 0xffffffffffffffffULL),
          "out-of-range doubleword access rejected");
    std::uint8_t byte = 0;
    check(mem.write_u8(7, 0xabU) && mem.read_u8(7, byte) && byte == 0xabU,
          "byte access succeeds at the final guest address");
    check(!mem.read_u8(8, byte) && !mem.write_u8(8, 0xffU),
          "out-of-range byte access rejected");
}

} // namespace

int main() {
    test_decode_and_execute_arithmetic();
    test_carry_and_overflow_arithmetic();
    test_addze_arithmetic();
    test_negate_arithmetic();
    test_carry_immediate_and_subtract_arithmetic();
    test_logical_shift_and_rotate_instructions();
    test_doubleword_shift_instructions();
    test_word_shift_immediate_instruction();
    test_word_rotate_insert_and_variable_mask();
    test_doubleword_rotate_mask_instructions();
    test_variable_doubleword_rotate_mask_instructions();
    test_leading_zero_instructions();
    test_memory_and_endianness();
    test_byte_and_halfword_memory_instructions();
    test_load_store_update_instructions();
    test_doubleword_memory_instructions();
    test_floating_point_load_store_transfers();
    test_floating_point_sign_and_move_instructions();
    test_indexed_memory_instructions();
    test_indexed_update_memory_instructions();
    test_byte_reversed_indexed_memory_instructions();
    test_multiple_word_memory_instructions();
    test_integer_divide_instructions();
    test_integer_multiply_instructions();
    test_integer_high_word_multiply_instructions();
    test_integer_high_doubleword_multiply_instructions();
    test_sign_extension_instructions();
    test_immediate_logic_instructions();
    test_xor_and_immediate_logic_instructions();
    test_branch_and_faults();
    test_program_exception_and_rfid_return();
    test_system_call_exception_and_return();
    test_cpu_fetch_from_mapped_executable_page();
    test_compare_immediates();
    test_compare_registers();
    test_condition_register_transfers();
    test_move_condition_register_field();
    test_condition_register_logical_operations();
    test_special_register_transfers();
    test_move_condition_register_from_xer();
    test_conditional_branches();
    test_absolute_and_register_branches();
    test_memory_bounds_and_alignment();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All Project360 tests passed\n";
    return 0;
}
