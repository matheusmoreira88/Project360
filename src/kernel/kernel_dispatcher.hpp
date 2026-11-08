#pragma once

#include "cpu/cpu_state.hpp"

#include <cstdint>
#include <string_view>

namespace project360::memory {
class GuestMemory;
}

namespace project360::kernel {

struct KernelImport {
    std::string_view library;
    std::uint16_t ordinal{};
};

enum class KernelDispatchStatus : std::uint8_t {
    handled,
    unsupported_library,
    unsupported_export,
    invalid_guest_argument,
    guest_memory_fault,
};

class KernelDispatcher {
public:
    static constexpr std::uint16_t rtl_compare_memory_ordinal = 268;
    static constexpr std::uint16_t rtl_compare_memory_ulong_ordinal = 269;
    static constexpr std::uint16_t rtl_compare_string_ordinal = 270;
    static constexpr std::uint16_t rtl_char_to_integer_ordinal = 267;
    static constexpr std::uint16_t rtl_fill_memory_ordinal = 284;
    static constexpr std::uint16_t rtl_fill_memory_ulong_ordinal = 285;
    static constexpr std::uint16_t rtl_init_ansi_string_ordinal = 289;
    static constexpr std::uint16_t rtl_init_unicode_string_ordinal = 290;
    static constexpr std::uint16_t rtl_integer_to_unicode_string_ordinal = 293;
    static constexpr std::uint16_t rtl_move_memory_ordinal = 298;
    static constexpr std::uint16_t rtl_nt_status_to_dos_error_ordinal = 301;
    static constexpr std::uint16_t rtl_ulong_byte_swap_ordinal = 307;
    static constexpr std::uint16_t rtl_ushort_byte_swap_ordinal = 318;
    static constexpr std::uint16_t rtl_zero_memory_ordinal = 320;
    static constexpr std::uint16_t rtl_unicode_string_to_integer_ordinal = 309;

    // Dispatches an imported xboxkrnl function using the Xenon register ABI.
    // A handled void function returns to the caller through LR (blr semantics).
    [[nodiscard]] KernelDispatchStatus dispatch(KernelImport import,
                                                cpu::CpuState& state,
                                                memory::GuestMemory& memory) const noexcept;
};

} // namespace project360::kernel
