#include "cpu/cpu_state.hpp"
#include "cpu/decoder.hpp"
#include "cpu/interpreter.hpp"
#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>

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
              fault_state.pc == 0,
          "unaligned ld faults without changing target or PC");

    memory::GuestMemory update_fault_mem(64);
    put_word(update_fault_mem, 0, ds_form(58, 4, 1, 8, 1)); // ldu r4,8(r1)
    cpu::CpuState update_fault_state{};
    update_fault_state.gpr[1] = 56;
    update_fault_state.gpr[4] = 0x55;
    const auto update_fault = cpu::Interpreter{}.step(update_fault_state, update_fault_mem);
    check(update_fault.status == cpu::StepStatus::fault && update_fault_state.gpr[1] == 56 &&
              update_fault_state.gpr[4] == 0x55 && update_fault_state.pc == 0,
          "out-of-range ldu does not commit either destination or base register");
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
    check(state.gpr[4] == 0xffffffff00000000ULL,
          "mullw returns the full signed product of its low-word operands");
    check(state.gpr[7] == 42,
          "mullw with OE clears OV when the signed word product fits");
    check(state.gpr[10] == 0xfffffffffffffffeULL,
          "mulld returns the low doubleword of a signed 64-bit product");
    check(state.gpr[13] == 0,
          "mulld wraps the low doubleword for an overflowing negative product");
    check((state.xer & 0xe0000000U) == 0xe0000000U && (state.cr >> 28U) == 9U,
          "multiply overflow updates OV/SO and record updates CR0");
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
    check(fetch.status == cpu::StepStatus::fault, "out-of-range instruction fetch faults");

    check(!cpu::decode(branch_conditional(0x15U, 0, 4)),
          "reserved conditional branch option is diagnosed");
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
    test_carry_immediate_and_subtract_arithmetic();
    test_logical_shift_and_rotate_instructions();
    test_memory_and_endianness();
    test_byte_and_halfword_memory_instructions();
    test_load_store_update_instructions();
    test_doubleword_memory_instructions();
    test_indexed_memory_instructions();
    test_indexed_update_memory_instructions();
    test_byte_reversed_indexed_memory_instructions();
    test_multiple_word_memory_instructions();
    test_integer_multiply_instructions();
    test_sign_extension_instructions();
    test_immediate_logic_instructions();
    test_xor_and_immediate_logic_instructions();
    test_branch_and_faults();
    test_compare_immediates();
    test_compare_registers();
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
