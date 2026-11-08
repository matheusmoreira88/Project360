#include "kernel/kernel_dispatcher.hpp"
#include "memory/guest_memory.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kSource1 = 0x1000U;
constexpr std::uint32_t kSource2 = 0x2000000U;
constexpr std::uint64_t kReturnAddress = 0x100004U;

bool parse_positive(std::string_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    std::uint64_t parsed = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0U) return false;
    value = parsed;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t length = 1U << 20U;
    std::uint64_t iterations = 20U;
    if (argc > 3 || (argc >= 2 && !parse_positive(argv[1], length)) ||
        (argc >= 3 && !parse_positive(argv[2], iterations))) {
        std::cerr << "usage: project360_kernel_memory_bench [length_bytes] [iterations]\n";
        return 2;
    }
    if (length > 0x1000000U || iterations > 1000000U ||
        length > std::numeric_limits<std::uint32_t>::max() - kSource2) {
        std::cerr << "benchmark size exceeds the configured guest-address bounds\n";
        return 2;
    }

    const auto backing_size = static_cast<std::size_t>(kSource2) +
                              static_cast<std::size_t>(length);
    project360::memory::GuestMemory memory(backing_size);
    std::vector<std::byte> contents(static_cast<std::size_t>(length));
    for (std::size_t index = 0U; index < contents.size(); ++index) {
        const auto value = static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU);
        contents[index] = static_cast<std::byte>(value);
    }
    if (!memory.load(kSource1, contents) || !memory.load(kSource2, contents)) {
        std::cerr << "could not initialize guest memory for the benchmark\n";
        return 1;
    }

    project360::kernel::KernelDispatcher dispatcher;
    project360::cpu::CpuState state{};
    const auto execute_once = [&]() {
        state.gpr[3] = kSource1;
        state.gpr[4] = kSource2;
        state.gpr[5] = length;
        state.lr = kReturnAddress;
        return dispatcher.dispatch(
            {"xboxkrnl.exe", project360::kernel::KernelDispatcher::rtl_compare_memory_ordinal},
            state, memory);
    };

    for (std::uint32_t warmup = 0U; warmup < 3U; ++warmup) {
        if (execute_once() != project360::kernel::KernelDispatchStatus::handled ||
            state.gpr[3] != length) {
            std::cerr << "benchmark warmup did not complete a full matching comparison\n";
            return 1;
        }
    }

    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t iteration = 0U; iteration < iterations; ++iteration) {
        if (execute_once() != project360::kernel::KernelDispatchStatus::handled ||
            state.gpr[3] != length) {
            std::cerr << "benchmark comparison did not complete successfully\n";
            return 1;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsed_seconds = std::chrono::duration<double>(elapsed).count();
    const auto compared_mib = static_cast<double>(length) * static_cast<double>(iterations) /
                              (1024.0 * 1024.0);
    const auto throughput = compared_mib / elapsed_seconds;

    std::cout << "Project360 RtlCompareMemory benchmark\n"
#if defined(NDEBUG)
              << "build=Release\n"
#else
              << "build=Debug\n"
#endif
#if defined(_MSC_VER)
              << "compiler=MSVC " << _MSC_VER << '\n'
#elif defined(__clang__)
              << "compiler=Clang " << __clang_version__ << '\n'
#elif defined(__GNUC__)
              << "compiler=GCC " << __VERSION__ << '\n'
#else
              << "compiler=unknown\n"
#endif
              << "logical_bytes_per_comparison=" << length << '\n'
              << "iterations=" << iterations << '\n'
              << "host_hardware_threads=" << std::thread::hardware_concurrency() << '\n'
              << "elapsed_ms="
              << std::chrono::duration<double, std::milli>(elapsed).count() << '\n'
              << "logical_compare_throughput_mib_per_second=" << throughput << '\n';
    return 0;
}
