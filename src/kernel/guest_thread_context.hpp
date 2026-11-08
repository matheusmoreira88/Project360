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
    std::uint32_t thread_object_address{};
    std::uint32_t tls_address{};
    std::uint32_t tls_slot_address{};
    std::uint32_t tls_slot_count{};
    std::uint32_t stack_limit{};
    std::uint32_t stack_base{};
};

struct GuestThreadInitResult {
    std::optional<GuestMainThreadContext> context;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return context.has_value(); }
};

// Creates the bounded single-thread launch state used to enter a loaded XEX.
// The KTHREAD block contains only fields initialized by the current bootstrap
// model; this does not create scheduler, handle-table, or lifecycle state.
[[nodiscard]] GuestThreadInitResult initialize_main_thread_context(
    const loader::LoadedXexImage& image, memory::GuestMemory& memory,
    cpu::CpuState& state, std::uint32_t stack_size = 64U * 1024U);

} // namespace project360::kernel
