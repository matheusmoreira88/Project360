#include "cpu/interpreter.hpp"
#include "common/log.hpp"
#include "loader/xex_parser.hpp"
#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
    std::cout << "Encryption type: ";
    if (metadata.encryption_type) std::cout << std::dec << *metadata.encryption_type << '\n';
    else std::cout << "not present\n";
    std::cout << "Compression type: ";
    if (metadata.compression_type) std::cout << *metadata.compression_type << '\n';
    else std::cout << "not present\n";
    if (metadata.import_libraries.empty()) {
        std::cout << "Import libraries: not present\n";
    } else {
        std::cout << "Import libraries (metadata only):\n";
        for (const auto& library : metadata.import_libraries) {
            std::cout << "  " << library.name << " version=0x" << std::hex << std::uppercase
                      << library.version << " minimum=0x" << library.minimum_version
                      << std::dec << " records=" << library.import_record_addresses.size()
                      << '\n';
        }
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

int run_command_line(int argc, bool inspect, const std::filesystem::path& path) {
    if (inspect) return inspect_xex(path);
    if (argc != 1) {
        std::cerr << "Usage: project360 [--inspect-xex <path>]\n";
        return 2;
    }
    return run_smoke_program();
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    const bool inspect = argc == 3 && std::wstring_view(argv[1]) == L"--inspect-xex";
    const auto path = inspect ? std::filesystem::path(argv[2]) : std::filesystem::path{};
    return run_command_line(argc, inspect, path);
}
#else
int main(int argc, char** argv) {
    const bool inspect = argc == 3 && std::string_view(argv[1]) == "--inspect-xex";
    const auto path = inspect ? std::filesystem::path(argv[2]) : std::filesystem::path{};
    return run_command_line(argc, inspect, path);
}
#endif
