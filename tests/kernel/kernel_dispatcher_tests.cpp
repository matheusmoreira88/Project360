#include "kernel/kernel_dispatcher.hpp"

#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using project360::cpu::CpuState;
    using project360::kernel::KernelDispatchStatus;
    using project360::kernel::KernelDispatcher;
    using project360::kernel::KernelImport;
    using project360::memory::GuestMemory;
    using project360::memory::GuestPageAccess;

    constexpr std::uint64_t data_page = 0x200000U;
    GuestMemory memory(2U * static_cast<std::size_t>(GuestMemory::page_size));
    const std::array<std::byte, 4> initial{
        std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}, std::byte{0xdd}};
    check(memory.load(0U, initial), "test fixture initializes destination backing");
    check(memory.load(initial.size(), initial),
          "test fixture creates a second byte range for compare");
    check(memory.map_page(data_page, 0U,
                          GuestPageAccess::read | GuestPageAccess::write),
          "guest destination is mapped for the kernel service");

    CpuState state{};
    state.gpr[3] = data_page;
    state.gpr[4] = initial.size() * 2U;
    state.gpr[5] = 0xaabbccddU;
    state.lr = 0x1037U;
    state.pc = 0x800070U;
    const auto matching_words = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ulong_ordinal}, state, memory);
    check(matching_words == KernelDispatchStatus::handled && state.gpr[3] == 8U &&
              state.pc == 0x1034U,
          "xboxkrnl ordinal 269 returns the byte count for repeated matching ULONGs");
    state.gpr[3] = data_page + 1U;
    state.gpr[4] = 8U;
    state.gpr[5] = 0xaabbccddU;
    const auto unaligned_words = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ulong_ordinal}, state, memory);
    check(unaligned_words == KernelDispatchStatus::handled && state.gpr[3] == 0U &&
              state.pc == 0x1034U,
          "RtlCompareMemoryUlong returns zero for an unaligned source");
    state.gpr[3] = data_page;
    state.gpr[4] = 6U;
    state.gpr[5] = 0xaabbccddU;
    const auto partial_word_length = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ulong_ordinal}, state, memory);
    check(partial_word_length == KernelDispatchStatus::handled && state.gpr[3] == 0U &&
              state.pc == 0x1034U,
          "RtlCompareMemoryUlong returns zero for a length not divisible by four");

    state.gpr[3] = data_page;
    state.gpr[4] = data_page + initial.size();
    state.gpr[5] = initial.size();
    state.lr = 0x1027U;
    state.pc = 0x800080U;
    const auto matching = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state, memory);
    check(matching == KernelDispatchStatus::handled && state.gpr[3] == initial.size() &&
              state.pc == 0x1024U,
          "xboxkrnl ordinal 268 returns the full length for identical guest bytes");

    check(memory.write_u8(data_page + initial.size() + 2U, 0x99U),
          "test fixture changes a byte after the shared prefix");
    state.gpr[3] = data_page;
    state.gpr[4] = data_page + initial.size();
    state.gpr[5] = initial.size();
    const auto differing = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state, memory);
    check(differing == KernelDispatchStatus::handled && state.gpr[3] == 2U,
          "RtlCompareMemory returns the matching prefix length up to the first difference");

    state.gpr[3] = data_page;
    state.gpr[4] = 8U;
    state.gpr[5] = 0xaabbccddU;
    const auto differing_word = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ulong_ordinal}, state, memory);
    check(differing_word == KernelDispatchStatus::handled && state.gpr[3] == 4U,
          "RtlCompareMemoryUlong stops at the first mismatching word");

    state.gpr[3] = data_page;
    state.gpr[4] = 0x300000U;
    state.gpr[5] = 4U;
    state.pc = 0x800090U;
    const auto compare_fault = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state, memory);
    check(compare_fault == KernelDispatchStatus::guest_memory_fault &&
              state.pc == 0x800090U && state.gpr[3] == data_page,
          "RtlCompareMemory reports an inaccessible guest range without changing CPU state");

    state.gpr[3] = 0xfffffff0U;
    state.gpr[4] = 0x1000U;
    state.gpr[5] = 0x40U;
    const auto compare_wrap = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state, memory);
    check(compare_wrap == KernelDispatchStatus::invalid_guest_argument &&
              state.pc == 0x800090U,
          "RtlCompareMemory rejects guest address ranges that wrap the 32-bit address space");

    check(memory.load(32U, initial), "test fixture seeds a source range for RtlMoveMemory");
    state.gpr[3] = data_page + 40U;
    state.gpr[4] = data_page + 32U;
    state.gpr[5] = initial.size();
    state.lr = 0x1047U;
    state.pc = 0x8000a0U;
    const auto moved = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_move_memory_ordinal}, state, memory);
    bool moved_matches = true;
    for (std::size_t i = 0; i < initial.size(); ++i) {
        std::uint8_t byte = 0U;
        moved_matches = moved_matches && memory.read_u8(data_page + 40U + i, byte) &&
                        byte == std::to_integer<std::uint8_t>(initial[i]);
    }
    check(moved == KernelDispatchStatus::handled && moved_matches &&
              state.pc == 0x1044U,
          "xboxkrnl ordinal 298 copies guest bytes and returns through LR");

    state.gpr[3] = data_page;
    state.gpr[4] = initial.size();
    state.gpr[14] = 0x1122334455667788ULL;
    state.lr = 0x1237U;
    state.pc = 0x800100U;
    const auto handled = KernelDispatcher{}.dispatch(
        {"XBOXKRNL.EXE", KernelDispatcher::rtl_zero_memory_ordinal}, state, memory);
    std::uint32_t word = 1U;
    check(handled == KernelDispatchStatus::handled,
          "xboxkrnl ordinal 320 dispatches the implemented memory routine");
    check(memory.read_u32_be(data_page, word) && word == 0U,
          "RtlZeroMemory clears guest virtual memory");
    check(state.pc == 0x1234U && state.gpr[3] == data_page &&
              state.gpr[4] == initial.size() && state.gpr[14] == 0x1122334455667788ULL,
          "successful void import returns through LR and preserves argument/nonvolatile GPRs");

    state.gpr[3] = data_page;
    state.gpr[4] = initial.size();
    state.gpr[5] = 0x1234abU;
    state.lr = 0x2237U;
    const auto filled = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_fill_memory_ordinal}, state, memory);
    std::uint32_t filled_word = 0U;
    check(filled == KernelDispatchStatus::handled &&
              memory.read_u32_be(data_page, filled_word) && filled_word == 0xababababU,
          "xboxkrnl ordinal 284 fills guest memory with the low byte of GPR5");
    check(state.pc == 0x2234U,
          "RtlFillMemory returns through LR after filling its destination");

    state.gpr[3] = data_page;
    state.gpr[4] = 4U;
    state.gpr[5] = 0x11223344U;
    state.lr = 0x3237U;
    const auto filled_ulong = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_fill_memory_ulong_ordinal}, state, memory);
    check(filled_ulong == KernelDispatchStatus::handled &&
              memory.read_u32_be(data_page, word) && word == 0x11223344U &&
              state.pc == 0x3234U,
          "xboxkrnl ordinal 285 repeats the big-endian GPR5 word pattern");
    state.gpr[3] = data_page + 1U;
    state.pc = 0x800180U;
    const auto misaligned = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_fill_memory_ulong_ordinal}, state, memory);
    check(misaligned == KernelDispatchStatus::invalid_guest_argument &&
              state.pc == 0x800180U,
          "RtlFillMemoryUlong rejects unaligned guest ranges without returning");

    check(memory.load(GuestMemory::page_size - 2U, initial),
          "test fixture seeds both pages before checking failure atomicity");
    check(memory.map_page(data_page + GuestMemory::page_size, GuestMemory::page_size,
                          GuestPageAccess::read),
          "second destination page is read-only");
    state.gpr[3] = data_page + GuestMemory::page_size - 2U;
    state.gpr[4] = 4U;
    state.gpr[5] = 0x5aU;
    state.pc = 0x800200U;
    const auto fault = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_fill_memory_ordinal}, state, memory);
    std::uint8_t first_byte = 0;
    check(fault == KernelDispatchStatus::guest_memory_fault && state.pc == 0x800200U,
          "memory faults propagate without returning to the guest caller");
    check(memory.read_u8(data_page + GuestMemory::page_size - 2U, first_byte) &&
              first_byte == 0xaaU,
          "failed service does not modify writable bytes before a protected page");
    check(memory.read_u8(data_page + GuestMemory::page_size - 1U, first_byte) &&
              first_byte == 0xbbU,
          "failed RtlFillMemory preserves its entire destination range");

    state.gpr[3] = 0x1'0000'0000ULL;
    state.gpr[4] = 1U;
    const auto invalid = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_zero_memory_ordinal}, state, memory);
    check(invalid == KernelDispatchStatus::invalid_guest_argument && state.pc == 0x800200U,
          "out-of-width guest pointers are rejected without changing control flow");

    state.gpr[3] = data_page;
    state.gpr[4] = 1U;
    const auto unsupported = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", 0xffffU}, state, memory);
    check(unsupported == KernelDispatchStatus::unsupported_export && state.pc == 0x800200U,
          "unknown kernel ordinals report unsupported and keep CPU state intact");
    const auto wrong_library = KernelDispatcher{}.dispatch(
        {"xam.xex", KernelDispatcher::rtl_zero_memory_ordinal}, state, memory);
    check(wrong_library == KernelDispatchStatus::unsupported_library,
          "an ordinal from another import library is not misrouted to xboxkrnl");

    if (failures != 0) {
        std::cerr << failures << " kernel dispatcher test(s) failed\n";
        return 1;
    }
    std::cout << "All kernel dispatcher tests passed\n";
    return 0;
}
