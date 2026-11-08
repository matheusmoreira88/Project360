#pragma once

#include "cpu/cpu_state.hpp"
#include "memory/guest_memory.hpp"

#include <cstddef>
#include <string>

namespace project360::cpu {

enum class StepStatus { executed, instruction_limit_reached, fault };

struct StepResult {
    StepStatus status{StepStatus::executed};
    std::string message;
};

class Interpreter {
public:
    [[nodiscard]] StepResult step(CpuState& state, memory::GuestMemory& memory) const;
    [[nodiscard]] StepResult run(CpuState& state, memory::GuestMemory& memory,
                                 std::size_t instruction_limit) const;
};

} // namespace project360::cpu
