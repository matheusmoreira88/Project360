#include "cpu/interpreter.hpp"
#include "common/log.hpp"
#include "loader/xex_parser.hpp"
#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string_view>

int inspect_xex(const std::filesystem::path& path) {
    const auto result = project360::loader::parse_xex2_file(path);
    if (!result) {
        project360::common::log(project360::common::LogLevel::error, "loader", result.error);
        return 1;
    }

    const auto& metadata = *result.metadata;
    std::cout << "Project360 XEX2 metadata (image is not loaded or executed)\n"
              << "Module flags: 0x" << std::hex << std::uppercase << metadata.module_flags
              << "\nHeader size: 0x" << metadata.header_size
              << "\nImage size: 0x" << metadata.image_size << '\n';
    if (metadata.title_id) {
        std::cout << "Title ID: 0x" << *metadata.title_id << '\n';
    } else {
        std::cout << "Title ID: not present\n";
    }
    if (metadata.media_id) {
        std::cout << "Media ID: 0x" << *metadata.media_id << '\n';
    } else {
        std::cout << "Media ID: not present\n";
    }
    if (metadata.entry_point) {
        std::cout << "Entry point value: 0x" << *metadata.entry_point << '\n';
    } else {
        std::cout << "Entry point: not present in XEX optional headers\n";
    }
    return 0;
}

int run_smoke_program() {
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

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--inspect-xex") {
        return inspect_xex(std::filesystem::path(argv[2]));
    }
    if (argc != 1) {
        std::cerr << "Usage: project360 [--inspect-xex <path>]\n";
        return 2;
    }
    return run_smoke_program();
}
