#pragma once

#include "cpu/cpu_state.hpp"
#include "loader/xex_parser.hpp"
#include "memory/guest_memory.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace project360::kernel {

struct GuestMainThreadContext {
    std::uint32_t pcr_address{};
    std::uint32_t tls_address{};
    std::uint32_t stack_limit{};
    std::uint32_t stack_base{};
};

struct GuestThreadInitResult {
    std::optional<GuestMainThreadContext> context;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return context.has_value(); }
};

// Creates the bounded single-thread launch state used to enter a loaded XEX.
// It initializes stack, static TLS and the documented KPCR fields, but does not
// create a kernel thread object or scheduler state.
[[nodiscard]] GuestThreadInitResult initialize_main_thread_context(
    const loader::LoadedXexImage& image, memory::GuestMemory& memory,
    cpu::CpuState& state, std::uint32_t stack_size = 64U * 1024U);

} // namespace project360::kernel
