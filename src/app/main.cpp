#include "cpu/interpreter.hpp"
#include "common/log.hpp"
#include "kernel/guest_executor.hpp"
#include "loader/xex_parser.hpp"
#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <new>
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
    if (metadata.title_version) {
        std::cout << "Title version: 0x" << *metadata.title_version << '\n';
    }
    if (metadata.base_version) {
        std::cout << "Base version: 0x" << *metadata.base_version << '\n';
    }
    if (metadata.platform) {
        std::cout << "Platform: " << std::dec << static_cast<unsigned>(*metadata.platform) << '\n';
    }
    if (metadata.executable_type) {
        std::cout << "Executable type: " << std::dec
                  << static_cast<unsigned>(*metadata.executable_type) << '\n';
    }
    if (metadata.disc_number && metadata.disc_count) {
        std::cout << "Disc: " << std::dec << static_cast<unsigned>(*metadata.disc_number) << "/"
                  << static_cast<unsigned>(*metadata.disc_count) << '\n';
    }
    if (metadata.savegame_id) {
        std::cout << "Savegame ID: 0x" << std::hex << std::uppercase << *metadata.savegame_id
                  << '\n';
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

int run_xex(const std::filesystem::path& path) {
    constexpr std::size_t guest_memory_size = 512U * 1024U * 1024U;
    constexpr std::uint64_t instruction_limit = 1'000'000U;
    const auto metadata = project360::loader::parse_xex2_file(path);
    if (!metadata) {
        project360::common::log(project360::common::LogLevel::error, "loader",
                                metadata.error);
        return 1;
    }
    if (!metadata.metadata->encryption_type ||
        *metadata.metadata->encryption_type != 0U) {
        project360::common::log(
            project360::common::LogLevel::error, "loader",
            "XEX execution supports only unencrypted images");
        return 1;
    }

    try {
        project360::memory::GuestMemory memory(guest_memory_size);
        const auto loaded = project360::loader::load_xex2_file(path, memory, 0U);
        if (!loaded) {
            project360::common::log(project360::common::LogLevel::error, "loader",
                                    loaded.error);
            return 1;
        }

        project360::cpu::CpuState state{};
        state.pc = loaded.image->entry_point;
        const project360::kernel::GuestExecutor executor(*loaded.image);
        const auto result = executor.run(state, memory, instruction_limit);
        std::cout << "XEX execution stopped after " << result.instructions_executed
                  << " guest instructions at PC 0x" << std::hex << std::uppercase
                  << state.pc << std::dec << ".\n" << result.stop.message << '\n';
        if (result.stop.status == project360::cpu::StepStatus::instruction_limit_reached) {
            std::cout << "The instruction budget is a diagnostic limit; this does not mean the title is running.\n";
            return 3;
        }
        return result.stop.status == project360::cpu::StepStatus::fault ? 1 : 0;
    } catch (const std::bad_alloc&) {
        project360::common::log(project360::common::LogLevel::error, "memory",
                                "unable to reserve 512 MiB for guest memory");
        return 1;
    }
}

int run_command_line(int argc, bool inspect, bool run, const std::filesystem::path& path) {
    if (inspect) return inspect_xex(path);
    if (run) return run_xex(path);
    if (argc != 1) {
        std::cerr << "Usage: project360 [--inspect-xex <path> | --run-xex <path>]\n";
        return 2;
    }
    return run_smoke_program();
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    const bool inspect = argc == 3 && std::wstring_view(argv[1]) == L"--inspect-xex";
    const bool run = argc == 3 && std::wstring_view(argv[1]) == L"--run-xex";
    const auto path = (inspect || run) ? std::filesystem::path(argv[2])
                                       : std::filesystem::path{};
    return run_command_line(argc, inspect, run, path);
}
#else
int main(int argc, char** argv) {
    const bool inspect = argc == 3 && std::string_view(argv[1]) == "--inspect-xex";
    const bool run = argc == 3 && std::string_view(argv[1]) == "--run-xex";
    const auto path = (inspect || run) ? std::filesystem::path(argv[2])
                                       : std::filesystem::path{};
    return run_command_line(argc, inspect, run, path);
}
#endif
