#include "loader/xex_parser.hpp"

#include "cpu/cpu_state.hpp"
#include "cpu/interpreter.hpp"
#include "memory/guest_memory.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void write_be32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>(value & 0xffU);
}

void write_be16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void write_le16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

std::vector<std::byte> make_xex2() {
    std::vector<std::byte> bytes(0x1000U);
    write_be32(bytes, 0x00U, 0x58455832U);
    write_be32(bytes, 0x04U, 0x00000081U);
    write_be32(bytes, 0x08U, 0x1000U);
    write_be32(bytes, 0x10U, 0x100U);
    write_be32(bytes, 0x14U, 4U);
    write_be32(bytes, 0x18U, 0x00010100U); // Entry point (immediate).
    write_be32(bytes, 0x1cU, 0x83000U);
    write_be32(bytes, 0x20U, 0x00040006U); // Execution info (24-byte payload).
    write_be32(bytes, 0x24U, 0x300U);
    write_be32(bytes, 0x28U, 0x000003ffU); // File-format info (offset payload).
    write_be32(bytes, 0x2cU, 0x380U);
    write_be32(bytes, 0x30U, 0x00010201U); // Image base (immediate/pointer field).
    write_be32(bytes, 0x34U, 0x82000U);
    write_be32(bytes, 0x100U, 0x184U); // Security-info size.
    write_be32(bytes, 0x104U, 0x6000U); // Image size.
    write_be32(bytes, 0x210U, 0x82000U); // Security-info load address.
    write_be32(bytes, 0x280U, 0U);      // Page-descriptor count.
    write_be32(bytes, 0x300U, 0x12345678U); // Media ID.
    write_be32(bytes, 0x30cU, 0x545408A7U); // Title ID.
    write_be32(bytes, 0x380U, 8U);
    write_be32(bytes, 0x384U, 0U); // Encryption type and compression type are both zero.
    return bytes;
}

std::vector<std::byte> make_xbox_pe32() {
    std::vector<std::byte> image(0x300U);
    constexpr std::size_t nt = 0x80U;
    constexpr std::size_t file_header = nt + 4U;
    constexpr std::size_t optional_header = file_header + 20U;
    constexpr std::size_t section = optional_header + 224U;
    write_le16(image, 0U, 0x5a4dU);
    write_le32(image, 0x3cU, static_cast<std::uint32_t>(nt));
    write_le32(image, nt, 0x00004550U);
    write_le16(image, file_header, 0x01f2U);
    write_le16(image, file_header + 2U, 1U);
    write_le16(image, file_header + 16U, 224U);
    write_le16(image, file_header + 18U, 0x0100U);
    write_le16(image, optional_header, 0x010bU);
    write_le32(image, optional_header + 16U, 0x1000U);
    write_le32(image, optional_header + 28U, 0x82000U);
    write_le32(image, optional_header + 32U, 0x1000U);
    write_le32(image, optional_header + 36U, 0x200U);
    write_le32(image, optional_header + 56U, 0x2000U);
    write_le32(image, optional_header + 60U, 0x200U);
    write_le16(image, optional_header + 68U, 14U);
    const char section_name[] = ".text";
    for (std::size_t i = 0; i < sizeof(section_name) - 1U; ++i) {
        image[section + i] = static_cast<std::byte>(section_name[i]);
    }
    write_le32(image, section + 8U, 0x80U);
    write_le32(image, section + 12U, 0x1000U);
    write_le32(image, section + 16U, 0x100U);
    write_le32(image, section + 20U, 0x200U);
    write_le32(image, section + 36U, 0x60000020U);
    return image;
}

} // namespace

int main() {
    using project360::loader::parse_xex2;
    auto bytes = make_xex2();
    const auto parsed = parse_xex2(bytes);
    check(parsed && parsed.metadata->module_flags == 0x81U &&
              parsed.metadata->header_size == 0x1000U &&
              parsed.metadata->security_info_offset == 0x100U &&
              parsed.metadata->image_size == 0x6000U &&
              parsed.metadata->optional_header_count == 4U &&
              parsed.metadata->security_load_address == 0x82000U &&
              parsed.metadata->image_base_address == 0x82000U,
          "valid XEX2 headers expose bounded container metadata");
    check(parsed.metadata && parsed.metadata->title_id == 0x545408a7U &&
              parsed.metadata->media_id == 0x12345678U &&
              parsed.metadata->entry_point == 0x83000U &&
              parsed.metadata->encryption_type == 0U &&
              parsed.metadata->compression_type == 0U,
          "execution-info and entry-point headers are decoded as big-endian values");

    const std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + 20);
    check(!parse_xex2(truncated), "truncated fixed headers are rejected");

    auto bad_magic = bytes;
    write_be32(bad_magic, 0, 0x12345678U);
    check(!parse_xex2(bad_magic), "unknown file signatures are rejected");
    auto xex1 = bytes;
    write_be32(xex1, 0, 0x58455831U);
    check(!parse_xex2(xex1), "unsupported XEX1 format reports failure");

    auto oversized_header = bytes;
    write_be32(oversized_header, 8, 0xffffffffU);
    check(!parse_xex2(oversized_header), "oversized declared headers are rejected");
    auto truncated_directory = bytes;
    write_be32(truncated_directory, 20, 0xffffffffU);
    check(!parse_xex2(truncated_directory), "directory count overflow is rejected before iteration");
    auto bad_security_offset = bytes;
    write_be32(bad_security_offset, 16, 0xfffffff0U);
    check(!parse_xex2(bad_security_offset), "out-of-range security-info offsets are rejected");
    auto bad_security_size = bytes;
    write_be32(bad_security_size, 0x100U, 8U);
    check(!parse_xex2(bad_security_size), "undersized security-info blocks are rejected");
    auto bad_descriptor_count = bytes;
    write_be32(bad_descriptor_count, 0x280U, 0xffffffffU);
    check(!parse_xex2(bad_descriptor_count), "page descriptor counts are bounds-checked");
    auto bad_execution_offset = bytes;
    write_be32(bad_execution_offset, 0x24U, 0xfffffff0U);
    check(!parse_xex2(bad_execution_offset), "execution-info payload offsets are bounds-checked");
    auto duplicate_execution = bytes;
    write_be32(duplicate_execution, 0x18U, 0x00040006U);
    write_be32(duplicate_execution, 0x1cU, 0x300U);
    check(!parse_xex2(duplicate_execution), "duplicate execution-info headers are rejected");

    auto no_execution = bytes;
    write_be32(no_execution, 20, 0U);
    const auto without_ids = parse_xex2(no_execution);
    check(without_ids && !without_ids.metadata->title_id && !without_ids.metadata->media_id,
          "XEX2 containers without execution metadata remain parseable without invented IDs");

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto test_path = std::filesystem::temp_directory_path() /
                           ("Project360_XexParser_" + std::to_string(unique) + ".xex");
    {
        std::ofstream output(test_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    const auto parsed_file = project360::loader::parse_xex2_file(test_path);
    check(parsed_file && parsed_file.metadata->title_id == 0x545408a7U,
          "file parser reads only the bounded XEX header and extracts metadata");
    {
        std::ofstream output(test_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), 0x400);
    }
    check(!project360::loader::parse_xex2_file(test_path),
          "file parser rejects a truncated declared header");
    std::error_code remove_error;
    std::filesystem::remove(test_path, remove_error);

    auto pe = make_xbox_pe32();
    const auto parsed_pe = project360::loader::parse_xbox_pe32(pe);
    check(parsed_pe && parsed_pe.metadata->entry_point_rva == 0x1000U &&
              parsed_pe.metadata->image_base == 0x82000U &&
              parsed_pe.metadata->sections.size() == 1U &&
              parsed_pe.metadata->sections[0].virtual_address == 0x1000U,
          "Xbox PowerPC PE32 headers and section metadata are decoded safely");
    auto bad_pe_machine = pe;
    write_le16(bad_pe_machine, 0x84U, 0x014cU);
    check(!project360::loader::parse_xbox_pe32(bad_pe_machine),
          "non-PowerPC PE machine types are rejected");
    auto bad_pe_entry = pe;
    write_le32(bad_pe_entry, 0x98U + 16U, 0x1800U);
    check(!project360::loader::parse_xbox_pe32(bad_pe_entry),
          "PE entry points outside executable sections are rejected");
    auto bad_pe_section = pe;
    write_le32(bad_pe_section, 0x178U + 20U, 0xfffffff0U);
    check(!project360::loader::parse_xbox_pe32(bad_pe_section),
          "PE section raw ranges outside the image are rejected");

    auto loadable_pe = make_xbox_pe32();
    write_be32(loadable_pe, 0x200U, (14U << 26U) | (3U << 21U) | 5U); // addi r3,r0,5
    auto loadable_xex = make_xex2();
    loadable_xex.insert(loadable_xex.end(), loadable_pe.begin(), loadable_pe.end());
    project360::memory::GuestMemory guest_memory(0x4000U);
    const auto loaded = project360::loader::load_xex2_uncompressed(loadable_xex,
                                                                    guest_memory, 0U);
    check(loaded && loaded.image->image_base == 0x82000U &&
              loaded.image->entry_point == 0x83000U && loaded.image->mapped_size == 0x2000U,
          "unencrypted uncompressed XEX2 maps decoded Xbox PE image pages");
    std::uint32_t loaded_instruction = 0;
    check(guest_memory.fetch_u32_be(0x83000U, loaded_instruction) &&
              loaded_instruction == ((14U << 26U) | (3U << 21U) | 5U),
          "loader copies executable PE section bytes to the mapped guest address");
    std::uint8_t loaded_byte = 0;
    check(guest_memory.read_u8(0x82000U, loaded_byte) && loaded_byte == 'M' &&
              !guest_memory.fetch_u32_be(0x82000U, loaded_instruction) &&
              !guest_memory.write_u8(0x83000U, 0U),
          "loader gives headers read-only pages and applies section execute/write permissions");
    project360::cpu::CpuState guest_cpu{};
    guest_cpu.pc = loaded.image ? loaded.image->entry_point : 0U;
    const auto executed = project360::cpu::Interpreter{}.step(guest_cpu, guest_memory);
    check(executed.status == project360::cpu::StepStatus::executed &&
              guest_cpu.gpr[3] == 5U && guest_cpu.pc == 0x83004U,
          "interpreter executes a homebrew instruction from a loaded XEX test image");

    {
        std::ofstream output(test_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(loadable_xex.data()),
                     static_cast<std::streamsize>(loadable_xex.size()));
    }
    project360::memory::GuestMemory file_guest_memory(0x4000U);
    const auto loaded_from_file = project360::loader::load_xex2_file(
        test_path, file_guest_memory, 0U);
    check(loaded_from_file && loaded_from_file.image->entry_point == 0x83000U,
          "bounded file loader reads and maps a synthetic unencrypted XEX image");
    std::filesystem::remove(test_path, remove_error);

    auto encrypted_xex = loadable_xex;
    write_be16(encrypted_xex, 0x384U, 1U);
    project360::memory::GuestMemory rejected_memory(0x4000U);
    const auto encrypted_result = project360::loader::load_xex2_uncompressed(
        encrypted_xex, rejected_memory, 0U);
    check(!encrypted_result && !rejected_memory.page_mapping_enabled(),
          "encrypted XEX images are rejected without mapping or decrypting any bytes");

    auto compressed_xex = loadable_xex;
    write_be16(compressed_xex, 0x386U, 1U);
    project360::memory::GuestMemory compressed_memory(0x4000U);
    const auto compressed_result = project360::loader::load_xex2_uncompressed(
        compressed_xex, compressed_memory, 0U);
    check(!compressed_result && !compressed_memory.page_mapping_enabled(),
          "compressed XEX images are rejected before changing guest mappings");

    auto relocated_xex = loadable_xex;
    write_be32(relocated_xex, 0x34U, 0x84000U);
    project360::memory::GuestMemory relocation_memory(0x4000U);
    const auto relocation_result = project360::loader::load_xex2_uncompressed(
        relocated_xex, relocation_memory, 0U);
    check(!relocation_result && !relocation_memory.page_mapping_enabled(),
          "loader rejects a mismatched image base until relocation support exists");

    project360::memory::GuestMemory overlapping_memory(0x4000U);
    check(overlapping_memory.map_page(0x90000U, 0U,
                                      project360::memory::GuestPageAccess::read),
          "fixture reserves an existing physical guest page");
    const auto overlapping_result = project360::loader::load_xex2_uncompressed(
        loadable_xex, overlapping_memory, 0U);
    check(!overlapping_result && overlapping_memory.page_mapped(0x90000U) &&
              !overlapping_memory.page_mapped(0x82000U),
          "loader rejects occupied physical backing without changing existing mappings");

    project360::memory::GuestMemory small_memory(0x1000U);
    const auto small_memory_result = project360::loader::load_xex2_uncompressed(
        loadable_xex, small_memory, 0U);
    check(!small_memory_result && !small_memory.page_mapping_enabled(),
          "insufficient backing memory is rejected before changing the guest map");

    if (failures != 0) {
        std::cerr << failures << " XEX parser test(s) failed\n";
        return 1;
    }
    std::cout << "All XEX parser tests passed\n";
    return 0;
}
