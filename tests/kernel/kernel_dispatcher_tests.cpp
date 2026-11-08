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
    check(memory.map_page(data_page, 0U,
                          GuestPageAccess::read | GuestPageAccess::write),
          "guest destination is mapped for the kernel service");

    CpuState state{};
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

    check(memory.load(GuestMemory::page_size - 2U, initial),
          "test fixture seeds both pages before checking failure atomicity");
    check(memory.map_page(data_page + GuestMemory::page_size, GuestMemory::page_size,
                          GuestPageAccess::read),
          "second destination page is read-only");
    state.gpr[3] = data_page + GuestMemory::page_size - 2U;
    state.gpr[4] = 4U;
    state.pc = 0x800200U;
    const auto fault = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_zero_memory_ordinal}, state, memory);
    std::uint8_t first_byte = 0;
    check(fault == KernelDispatchStatus::guest_memory_fault && state.pc == 0x800200U,
          "memory faults propagate without returning to the guest caller");
    check(memory.read_u8(data_page + GuestMemory::page_size - 2U, first_byte) &&
              first_byte == 0xaaU,
          "failed service does not modify writable bytes before a protected page");
    check(memory.read_u8(data_page + GuestMemory::page_size - 1U, first_byte) &&
              first_byte == 0xbbU,
          "failed cross-page service preserves its entire destination range");

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
