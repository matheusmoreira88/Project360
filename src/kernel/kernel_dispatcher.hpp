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
    static constexpr std::uint16_t rtl_zero_memory_ordinal = 320;

    // Dispatches an imported xboxkrnl function using the Xenon register ABI.
    // A handled void function returns to the caller through LR (blr semantics).
    [[nodiscard]] KernelDispatchStatus dispatch(KernelImport import,
                                                cpu::CpuState& state,
                                                memory::GuestMemory& memory) const noexcept;
};

} // namespace project360::kernel
