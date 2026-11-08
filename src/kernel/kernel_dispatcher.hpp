#pragma once

#include "cpu/cpu_state.hpp"
#include "kernel/guest_pool.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace project360::memory {
class GuestMemory;
}

namespace project360::kernel {

struct KernelImport {
    std::string_view library;
    std::uint16_t ordinal{};
};

struct KernelTlsStorage {
    std::uint32_t slots_address{};
    std::uint32_t slot_count{};
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
    static constexpr std::uint16_t ex_allocate_pool_ordinal = 9;
    static constexpr std::uint16_t ex_allocate_pool_with_tag_ordinal = 10;
    static constexpr std::uint16_t ex_free_pool_ordinal = 15;
    static constexpr std::uint16_t rtl_compare_memory_ordinal = 282;
    static constexpr std::uint16_t rtl_compare_memory_ulong_ordinal = 283;
    static constexpr std::uint16_t rtl_compare_string_ordinal = 284;
    static constexpr std::uint16_t rtl_compare_unicode_string_ordinal = 286;
    static constexpr std::uint16_t rtl_downcase_unicode_char_ordinal = 292;
    static constexpr std::uint16_t rtl_fill_memory_ulong_ordinal = 294;
    static constexpr std::uint16_t rtl_init_ansi_string_ordinal = 300;
    static constexpr std::uint16_t rtl_init_unicode_string_ordinal = 301;
    static constexpr std::uint16_t rtl_nt_status_to_dos_error_ordinal = 309;
    static constexpr std::uint16_t rtl_upcase_unicode_char_ordinal = 329;
    static constexpr std::uint16_t ke_query_interrupt_time_ordinal = 130;
    static constexpr std::uint16_t ke_query_system_time_ordinal = 132;
    static constexpr std::uint16_t ke_query_performance_frequency_ordinal = 131;
    static constexpr std::uint16_t ke_tls_alloc_ordinal = 338;
    static constexpr std::uint16_t ke_tls_free_ordinal = 339;
    static constexpr std::uint16_t ke_tls_get_value_ordinal = 340;
    static constexpr std::uint16_t ke_tls_set_value_ordinal = 341;
    static constexpr std::uint32_t maximum_tls_slots = 2048U;
    static constexpr std::uint64_t xbox_360_performance_frequency = 49'875'000U;

    explicit KernelDispatcher(KernelTlsStorage tls_storage = {},
                             KernelPoolStorage pool_storage = {}) noexcept;

    // Dispatches an imported xboxkrnl function using the Xenon register ABI.
    // A handled void function returns to the caller through LR (blr semantics).
    [[nodiscard]] KernelDispatchStatus dispatch(KernelImport import,
                                                cpu::CpuState& state,
                                                memory::GuestMemory& memory) const noexcept;

private:
    KernelTlsStorage tls_storage_{};
    std::uint32_t tls_slot_limit_{};
    const std::chrono::steady_clock::time_point guest_start_time_;
    mutable GuestPoolAllocator pool_allocator_;
    mutable std::mutex tls_mutex_;
    mutable std::array<std::uint64_t, maximum_tls_slots / 64U> allocated_tls_slots_{};
};

} // namespace project360::kernel
