#pragma once

#include "cpu/cpu_state.hpp"
#include "memory/guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace project360::cpu {

enum class StepStatus { executed, instruction_limit_reached, fault, exception_taken };

enum class FaultKind {
    illegal_instruction,
    unsupported_instruction,
    privileged_instruction,
    trap,
    instruction_alignment,
    instruction_access,
    data_alignment,
    data_access,
    interpreter_internal
};

enum class GuestExceptionKind { program, system_call };

struct GuestException {
    GuestExceptionKind kind{};
    std::uint64_t vector{};
};

struct ExecutionFault {
    FaultKind kind{};
    std::uint64_t pc{};
    std::optional<std::uint32_t> instruction;
    std::optional<std::uint64_t> effective_address;
};

struct StepResult {
    StepStatus status{StepStatus::executed};
    std::string message;
    std::optional<ExecutionFault> fault_info;
    std::optional<GuestException> guest_exception;
};

class Interpreter {
public:
    [[nodiscard]] StepResult step(CpuState& state, memory::GuestMemory& memory) const;
    [[nodiscard]] StepResult run(CpuState& state, memory::GuestMemory& memory,
                                 std::size_t instruction_limit) const;
};

} // namespace project360::cpu
