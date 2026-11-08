#include "cpu/interpreter.hpp"
#include "common/log.hpp"
#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

int main() {
    project360::memory::GuestMemory memory(4096);
    // addi r3, r0, 7; addi r4, r3, 5; add r5, r3, r4
    constexpr std::array<std::uint32_t, 3> words{
        (14U << 26U) | (3U << 21U) | 7U,
        (14U << 26U) | (4U << 21U) | (3U << 16U) | 5U,
        (31U << 26U) | (5U << 21U) | (3U << 16U) | (4U << 11U) | (266U << 1U),
    };
    for (std::size_t i = 0; i < words.size(); ++i) {
        (void)memory.write_u32_be(i * 4U, words[i]);
    }
    project360::cpu::CpuState state{};
    const auto result = project360::cpu::Interpreter{}.run(state, memory, words.size());
    if (result.status == project360::cpu::StepStatus::fault) {
        project360::common::log(project360::common::LogLevel::error, "cpu", result.message);
        return 1;
    }
    std::cout << "Project360 PowerPC smoke program: r3=" << state.gpr[3]
              << " r4=" << state.gpr[4] << " r5=" << state.gpr[5] << '\n';
    return 0;
}
