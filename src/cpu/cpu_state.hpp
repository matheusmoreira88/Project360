#pragma once

#include <array>
#include <cstdint>

namespace project360::cpu {

struct CpuState {
    std::array<std::uint64_t, 32> gpr{};
    std::uint64_t pc{};
    std::uint64_t lr{};
    std::uint64_t ctr{};
    std::uint64_t msr{};
    std::uint64_t srr0{};
    std::uint64_t srr1{};
    std::uint32_t cr{};
    std::uint32_t xer{};
};

} // namespace project360::cpu
