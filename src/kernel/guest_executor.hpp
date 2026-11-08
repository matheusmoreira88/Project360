#pragma once

#include "cpu/interpreter.hpp"
#include "kernel/kernel_dispatcher.hpp"
#include "loader/xex_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace project360::kernel {

struct GuestRunResult {
    std::uint64_t instructions_executed{};
    cpu::StepResult stop;
};

// Executes a loaded XEX and intercepts function-import thunks at their guest
// addresses. This keeps import calls inside normal PPC control flow while the
// kernel export runs as a host shim.
class GuestExecutor {
public:
    explicit GuestExecutor(const loader::LoadedXexImage& image);

    [[nodiscard]] cpu::StepResult step(cpu::CpuState& state,
                                       memory::GuestMemory& memory) const;
    [[nodiscard]] GuestRunResult run(cpu::CpuState& state,
                                     memory::GuestMemory& memory,
                                     std::uint64_t instruction_limit) const;

private:
    struct ImportAddress {
        std::uint32_t address{};
        std::uint32_t library_index{};
        std::uint32_t import_index{};
    };

    const loader::LoadedXexImage& image_;
    std::vector<ImportAddress> import_addresses_;
    std::string initialization_error_;
    cpu::Interpreter interpreter_;
    KernelDispatcher dispatcher_;
};

} // namespace project360::kernel
