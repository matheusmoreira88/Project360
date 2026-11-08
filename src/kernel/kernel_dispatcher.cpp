#include "kernel/kernel_dispatcher.hpp"

#include "memory/guest_memory.hpp"

#include <cstddef>
#include <string_view>

namespace project360::kernel {
namespace {

[[nodiscard]] constexpr char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

[[nodiscard]] bool equals_ascii_case_insensitive(std::string_view left,
                                                  std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (ascii_lower(left[i]) != ascii_lower(right[i])) return false;
    }
    return true;
}

} // namespace

KernelDispatchStatus KernelDispatcher::dispatch(KernelImport import, cpu::CpuState& state,
                                                memory::GuestMemory& memory) const noexcept {
    if (!equals_ascii_case_insensitive(import.library, "xboxkrnl.exe")) {
        return KernelDispatchStatus::unsupported_library;
    }
    if (import.ordinal != rtl_fill_memory_ordinal &&
        import.ordinal != rtl_zero_memory_ordinal) {
        return KernelDispatchStatus::unsupported_export;
    }

    // RtlFillMemory(PVOID Destination, SIZE_T Length, UCHAR Fill) and
    // RtlZeroMemory(PVOID Destination, SIZE_T Length) are void imports. Xbox
    // 360 XEX code uses 32-bit guest pointers/ULONG-sized lengths in GPR3/GPR4.
    const auto destination = state.gpr[3];
    const auto length = state.gpr[4];
    const auto fill_value = import.ordinal == rtl_fill_memory_ordinal
                                ? static_cast<std::uint8_t>(state.gpr[5] & 0xffU)
                                : 0U;
    if (destination > 0xffff'ffffULL || length > 0xffff'ffffULL) {
        return KernelDispatchStatus::invalid_guest_argument;
    }
    if (!memory.fill(destination, static_cast<std::size_t>(length),
                     static_cast<std::byte>(fill_value))) {
        return KernelDispatchStatus::guest_memory_fault;
    }

    // A successful imported leaf call returns like `blr`; failed/unsupported
    // services leave the architectural state unchanged for diagnostics.
    state.pc = state.lr & ~std::uint64_t{3};
    return KernelDispatchStatus::handled;
}

} // namespace project360::kernel
