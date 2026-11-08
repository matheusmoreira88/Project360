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
    if (import.ordinal == rtl_compare_memory_ordinal) {
        const auto source1 = state.gpr[3];
        const auto source2 = state.gpr[4];
        const auto length = state.gpr[5];
        constexpr auto guest_address_limit = std::uint64_t{0xffff'ffffU};
        if (source1 > guest_address_limit || source2 > guest_address_limit ||
            length > guest_address_limit ||
            (length != 0U &&
             (length - 1U > guest_address_limit - source1 ||
              length - 1U > guest_address_limit - source2))) {
            return KernelDispatchStatus::invalid_guest_argument;
        }

        std::uint64_t matching_bytes = 0U;
        for (; matching_bytes < length; ++matching_bytes) {
            std::uint8_t left = 0U;
            std::uint8_t right = 0U;
            if (!memory.read_u8(source1 + matching_bytes, left) ||
                !memory.read_u8(source2 + matching_bytes, right)) {
                return KernelDispatchStatus::guest_memory_fault;
            }
            if (left != right) break;
        }

        state.gpr[3] = matching_bytes;
        state.pc = state.lr & ~std::uint64_t{3};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal != rtl_fill_memory_ordinal &&
        import.ordinal != rtl_fill_memory_ulong_ordinal &&
        import.ordinal != rtl_zero_memory_ordinal) {
        return KernelDispatchStatus::unsupported_export;
    }

    // These void imports use GPR3/GPR4, with a fill value or pattern in GPR5.
    // Xenon pointers and SIZE_T values are 32-bit. RtlFillMemoryUlong requires
    // a word-aligned destination and a length divisible by four.
    const auto destination = state.gpr[3];
    const auto length = state.gpr[4];
    if (destination > 0xffff'ffffULL || length > 0xffff'ffffULL) {
        return KernelDispatchStatus::invalid_guest_argument;
    }
    bool memory_updated = false;
    if (import.ordinal == rtl_fill_memory_ulong_ordinal) {
        if ((destination & 3U) != 0U || (length & 3U) != 0U) {
            return KernelDispatchStatus::invalid_guest_argument;
        }
        memory_updated = memory.fill_u32_be(destination, static_cast<std::size_t>(length),
                                            static_cast<std::uint32_t>(state.gpr[5]));
    } else {
        const auto fill_value = import.ordinal == rtl_fill_memory_ordinal
                                    ? static_cast<std::uint8_t>(state.gpr[5] & 0xffU)
                                    : 0U;
        memory_updated = memory.fill(destination, static_cast<std::size_t>(length),
                                     static_cast<std::byte>(fill_value));
    }
    if (!memory_updated) {
        return KernelDispatchStatus::guest_memory_fault;
    }

    // A successful imported leaf call returns like `blr`; failed/unsupported
    // services leave the architectural state unchanged for diagnostics.
    state.pc = state.lr & ~std::uint64_t{3};
    return KernelDispatchStatus::handled;
}

} // namespace project360::kernel
