#include "loader/xex_parser.hpp"

#include "cpu/cpu_state.hpp"
#include "cpu/interpreter.hpp"
#include "kernel/guest_executor.hpp"
#include "memory/guest_memory.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
    write_be32(bytes, 0x14U, 5U);
    write_be32(bytes, 0x18U, 0x00010100U); // Entry point (immediate).
    write_be32(bytes, 0x1cU, 0x83000U);
    write_be32(bytes, 0x20U, 0x00040006U); // Execution info (24-byte payload).
    write_be32(bytes, 0x24U, 0x300U);
    write_be32(bytes, 0x28U, 0x000003ffU); // File-format info (offset payload).
    write_be32(bytes, 0x2cU, 0x380U);
    write_be32(bytes, 0x30U, 0x00010201U); // Image base (immediate/pointer field).
    write_be32(bytes, 0x34U, 0x82000U);
    write_be32(bytes, 0x38U, 0x000103ffU); // Import libraries (offset payload).
    write_be32(bytes, 0x3cU, 0x400U);
    write_be32(bytes, 0x100U, 0x184U); // Security-info size.
    write_be32(bytes, 0x104U, 0x6000U); // Image size.
    write_be32(bytes, 0x210U, 0x82000U); // Security-info load address.
    write_be32(bytes, 0x280U, 0U);      // Page-descriptor count.
    write_be32(bytes, 0x300U, 0x12345678U); // Media ID.
    write_be32(bytes, 0x30cU, 0x545408A7U); // Title ID.
    write_be32(bytes, 0x380U, 8U);
    write_be32(bytes, 0x384U, 0U); // Encryption type and compression type are both zero.
    write_be32(bytes, 0x400U, 0x48U); // Import-libraries block size.
    write_be32(bytes, 0x404U, 16U);   // Aligned library-name table size.
    write_be32(bytes, 0x408U, 1U);    // One library name.
    constexpr char import_library_name[] = "xboxkrnl.exe";
    for (std::size_t i = 0; i < sizeof(import_library_name); ++i) {
        bytes[0x40cU + i] = static_cast<std::byte>(import_library_name[i]);
    }
    write_be32(bytes, 0x41cU, 0x2cU);        // Library descriptor size.
    write_be32(bytes, 0x434U, 0x11223344U);  // Library ID.
    write_be32(bytes, 0x438U, 0x00010002U);  // Library version.
    write_be32(bytes, 0x43cU, 0x00010000U);  // Minimum version.
    write_be16(bytes, 0x440U, 0U);           // String-table index.
    write_be16(bytes, 0x442U, 1U);           // One import record address.
    write_be32(bytes, 0x444U, 0x00083004U);  // Guest address of import record.
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

std::vector<std::byte> make_basic_compressed_xex(std::span<const std::byte> xex) {
    constexpr std::size_t header_size = 0x1000U;
    constexpr std::size_t pe_headers_size = 0x200U;
    constexpr std::size_t section_offset = 0x200U;
    constexpr std::size_t section_size = 0x140U;
    std::vector<std::byte> compressed(header_size + pe_headers_size + section_size);
    std::copy_n(xex.begin(), header_size, compressed.begin());
    std::copy_n(xex.begin() + header_size, pe_headers_size,
                compressed.begin() + header_size);
    std::copy_n(xex.begin() + header_size + section_offset, section_size,
                compressed.begin() + header_size + pe_headers_size);
    write_be32(compressed, 0x380U, 24U);
    write_be16(compressed, 0x386U, 1U);
    write_be32(compressed, 0x388U, 0x200U);
    write_be32(compressed, 0x38cU, 0xe00U);
    write_be32(compressed, 0x390U, 0x140U);
    write_be32(compressed, 0x394U, 0xec0U);
    return compressed;
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
              parsed.metadata->optional_header_count == 5U &&
              parsed.metadata->security_load_address == 0x82000U &&
              parsed.metadata->image_base_address == 0x82000U,
          "valid XEX2 headers expose bounded container metadata");
    check(parsed.metadata && parsed.metadata->import_libraries.size() == 1U &&
              parsed.metadata->import_libraries[0].name == "xboxkrnl.exe" &&
              parsed.metadata->import_libraries[0].library_index == 0U &&
              parsed.metadata->import_libraries[0].version == 0x00010002U &&
              parsed.metadata->import_libraries[0].minimum_version == 0x00010000U &&
              parsed.metadata->import_libraries[0].import_record_addresses.size() == 1U &&
              parsed.metadata->import_libraries[0].import_record_addresses[0] == 0x83004U,
          "XEX import metadata exposes a bounded library name, version and record address");
    check(parsed.metadata && parsed.metadata->title_id == 0x545408a7U &&
              parsed.metadata->media_id == 0x12345678U &&
              parsed.metadata->entry_point == 0x83000U &&
              parsed.metadata->encryption_type == 0U &&
              parsed.metadata->compression_type == 0U,
          "execution-info and entry-point headers are decoded as big-endian values");

    auto basic_metadata = bytes;
    write_be32(basic_metadata, 0x380U, 16U);
    write_be16(basic_metadata, 0x386U, 1U);
    write_be32(basic_metadata, 0x388U, 0x100U);
    write_be32(basic_metadata, 0x38cU, 0x200U);
    const auto parsed_basic = parse_xex2(basic_metadata);
    check(parsed_basic && parsed_basic.metadata->basic_compression_blocks.size() == 1U &&
              parsed_basic.metadata->basic_compression_blocks[0].data_size == 0x100U &&
              parsed_basic.metadata->basic_compression_blocks[0].zero_size == 0x200U,
          "XEX parser exposes bounded basic-compression data and zero-fill sizes");
    auto malformed_basic_metadata = basic_metadata;
    write_be32(malformed_basic_metadata, 0x380U, 15U);
    check(!parse_xex2(malformed_basic_metadata),
          "malformed basic-compression descriptor lengths are rejected");

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

    auto duplicate_imports = bytes;
    write_be32(duplicate_imports, 20U, 6U);
    write_be32(duplicate_imports, 0x40U, 0x000103ffU);
    write_be32(duplicate_imports, 0x44U, 0x400U);
    check(!parse_xex2(duplicate_imports), "duplicate XEX import-library headers are rejected");
    auto oversized_import_string_table = bytes;
    write_be32(oversized_import_string_table, 0x404U, 0xffffffffU);
    check(!parse_xex2(oversized_import_string_table),
          "oversized XEX import-library string tables are rejected");
    auto unterminated_import_name = bytes;
    for (std::size_t i = 0; i < 16U; ++i) {
        unterminated_import_name[0x40cU + i] = std::byte{0x41};
    }
    check(!parse_xex2(unterminated_import_name),
          "unterminated XEX import-library names are rejected");
    auto bad_import_descriptor_size = bytes;
    write_be32(bad_import_descriptor_size, 0x41cU, 0x20U);
    check(!parse_xex2(bad_import_descriptor_size),
          "undersized XEX import-library descriptors are rejected");
    auto excessive_import_count = bytes;
    write_be16(excessive_import_count, 0x442U, 2U);
    check(!parse_xex2(excessive_import_count),
          "XEX import counts cannot exceed their descriptor payload");

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
    auto pe_section_overlapping_headers = pe;
    write_le32(pe_section_overlapping_headers, 0x178U + 20U, 0x1f0U);
    check(!project360::loader::parse_xbox_pe32(pe_section_overlapping_headers),
          "PE section data overlapping declared headers is rejected");
    auto pe_overlapping_sections = pe;
    write_le16(pe_overlapping_sections, 0x84U + 2U, 2U);
    constexpr std::size_t second_section = 0x178U + 40U;
    write_le32(pe_overlapping_sections, second_section + 8U, 0x20U);
    write_le32(pe_overlapping_sections, second_section + 12U, 0x1000U);
    write_le32(pe_overlapping_sections, second_section + 16U, 0x20U);
    write_le32(pe_overlapping_sections, second_section + 20U, 0x280U);
    check(!project360::loader::parse_xbox_pe32(pe_overlapping_sections),
          "overlapping PE virtual section ranges are rejected");

    auto loadable_pe = make_xbox_pe32();
    write_be32(loadable_pe, 0x200U, (14U << 26U) | (3U << 21U) | 0x5000U);
    write_be32(loadable_pe, 0x204U, (14U << 26U) | (4U << 21U) | 8U);
    write_be32(loadable_pe, 0x208U, (14U << 26U) | (5U << 21U) | 0xabU);
    write_be32(loadable_pe, 0x20cU, 0x48000095U); // bl import thunk at +0x94.
    write_be32(loadable_pe, 0x210U, (14U << 26U) | (5U << 21U) | 0x1234U);
    write_be32(loadable_pe, 0x214U, 0x48000091U); // bl import thunk at +0x90.
    write_be32(loadable_pe, 0x218U, 0x48000091U); // bl import thunk at +0x90.
    write_be32(loadable_pe, 0x21cU, (14U << 26U) | (10U << 21U) | (3U << 16U));
    write_be32(loadable_pe, 0x220U, (14U << 26U) | (3U << 21U) | 0x5000U);
    write_be32(loadable_pe, 0x224U, (14U << 26U) | (4U << 21U) | 0x5004U);
    write_be32(loadable_pe, 0x228U, (14U << 26U) | (5U << 21U) | 4U);
    write_be32(loadable_pe, 0x22cU, 0x48000081U); // bl import thunk at +0x80.
    write_be32(loadable_pe, 0x230U, (14U << 26U) | (9U << 21U) | (3U << 16U));
    write_be32(loadable_pe, 0x234U, (14U << 26U) | (3U << 21U) | 0x5008U);
    write_be32(loadable_pe, 0x238U, (14U << 26U) | (4U << 21U) | 0x5000U);
    write_be32(loadable_pe, 0x23cU, (14U << 26U) | (5U << 21U) | 8U);
    write_be32(loadable_pe, 0x240U, 0x48000071U); // bl import thunk at +0x70.
    write_be32(loadable_pe, 0x244U, (14U << 26U) | (3U << 21U) | 0x5000U);
    write_be32(loadable_pe, 0x248U, (14U << 26U) | (4U << 21U) | 16U);
    write_be32(loadable_pe, 0x24cU, 0x48000069U); // bl import thunk at +0x68.
    write_be32(loadable_pe, 0x250U, (14U << 26U) | (6U << 21U) | 9U);
    write_be32(loadable_pe, 0x254U, (14U << 26U) | (3U << 21U) | 0x5080U);
    write_be32(loadable_pe, 0x258U, (14U << 26U) | (4U << 21U) | 0x5040U);
    write_be32(loadable_pe, 0x25cU, 0x4800005dU); // bl import thunk at +0x5c.
    write_be32(loadable_pe, 0x260U, (32U << 26U) | (7U << 21U) | 0x5080U);
    write_be32(loadable_pe, 0x264U, (32U << 26U) | (8U << 21U) | 0x5084U);
    write_be32(loadable_pe, 0x268U, (14U << 26U) | (3U << 21U) | 0x50a0U);
    write_be32(loadable_pe, 0x26cU, (14U << 26U) | (4U << 21U) | 0x5060U);
    write_be32(loadable_pe, 0x270U, 0x4800004dU); // bl import thunk at +0x4c.
    write_be32(loadable_pe, 0x274U, (32U << 26U) | (7U << 21U) | 0x50a0U);
    write_be32(loadable_pe, 0x278U, (32U << 26U) | (8U << 21U) | 0x50a4U);
    write_be32(loadable_pe, 0x27cU, (14U << 26U) | (3U << 21U) | 0x1234U);
    write_be32(loadable_pe, 0x280U, 0x48000041U); // bl import thunk at +0x40.
    write_be32(loadable_pe, 0x284U, (14U << 26U) | (9U << 21U) | (3U << 16U));
    write_be32(loadable_pe, 0x288U, (14U << 26U) | (3U << 21U) | 0x1234U);
    write_be32(loadable_pe, 0x28cU, 0x48000039U); // bl import thunk at +0x38.
    write_be32(loadable_pe, 0x290U, (14U << 26U) | (10U << 21U) | (3U << 16U));
    write_be32(loadable_pe, 0x294U, 0x48000038U); // b to the integer-parser call sequence.
    write_be32(loadable_pe, 0x2a0U, 0x0100011cU); // thunk, library 0, ordinal 284
    write_be32(loadable_pe, 0x2a4U, 0x0100011dU); // thunk, library 0, ordinal 285
    write_be32(loadable_pe, 0x2a8U, 0x0100010dU); // thunk, library 0, ordinal 269
    write_be32(loadable_pe, 0x2acU, 0x0100010cU); // thunk, library 0, ordinal 268
    write_be32(loadable_pe, 0x2b0U, 0x0100012aU); // thunk, library 0, ordinal 298
    write_be32(loadable_pe, 0x2b4U, 0x01000140U); // thunk, library 0, ordinal 320
    write_be32(loadable_pe, 0x2b8U, 0x01000121U); // thunk, library 0, ordinal 289
    write_be32(loadable_pe, 0x2bcU, 0x01000122U); // thunk, library 0, ordinal 290
    write_be32(loadable_pe, 0x2c0U, 0x01000133U); // thunk, library 0, ordinal 307
    write_be32(loadable_pe, 0x2c4U, 0x0100013eU); // thunk, library 0, ordinal 318
    write_be32(loadable_pe, 0x2c8U, 0x01000135U); // thunk, library 0, ordinal 309
    write_be32(loadable_pe, 0x2ccU, (14U << 26U) | (3U << 21U) | 0x50b0U);
    write_be32(loadable_pe, 0x2d0U, (14U << 26U) | (4U << 21U));
    write_be32(loadable_pe, 0x2d4U, (14U << 26U) | (5U << 21U) | 0x50b8U);
    write_be32(loadable_pe, 0x2d8U, 0x4bfffff1U); // bl import thunk at -0x10.
    write_be32(loadable_pe, 0x2dcU, 0x48000004U); // b to the next caller instruction at +0xe0.
    write_be32(loadable_pe, 0x2e0U, (14U << 26U) | (11U << 21U) | (3U << 16U));
    write_be32(loadable_pe, 0x2e4U, (14U << 26U) | (3U << 21U) | 42U);
    write_be32(loadable_pe, 0x2e8U, (14U << 26U) | (4U << 21U) | 16U);
    write_be32(loadable_pe, 0x2ecU, (14U << 26U) | (5U << 21U) | 0x50d0U);
    write_be32(loadable_pe, 0x2f0U, 0x4800000dU); // bl import thunk at +0x0c.
    write_be32(loadable_pe, 0x2f4U, (14U << 26U) | (12U << 21U) | (3U << 16U));
    write_be32(loadable_pe, 0x2fcU, 0x01000125U); // thunk, library 0, ordinal 293.
    loadable_pe.resize(0x340U);
    write_le32(loadable_pe, 0x178U + 8U, 0x140U);
    write_le32(loadable_pe, 0x178U + 16U, 0x140U);
    write_be32(loadable_pe, 0x300U, (14U << 26U) | (3U << 21U) | 0x5140U);
    write_be32(loadable_pe, 0x304U, (14U << 26U) | (4U << 21U) | 0x5150U);
    write_be32(loadable_pe, 0x308U, (14U << 26U) | (5U << 21U) | 1U);
    write_be32(loadable_pe, 0x30cU, 0x48000025U); // bl import thunk at +0x24.
    write_be32(loadable_pe, 0x310U, (14U << 26U) | (6U << 21U) | (3U << 16U));
    write_be32(loadable_pe, 0x314U, (14U << 26U) | (3U << 21U) | 0x5140U);
    write_be32(loadable_pe, 0x318U, (14U << 26U) | (4U << 21U) | 0x5150U);
    write_be32(loadable_pe, 0x31cU, (14U << 26U) | (5U << 21U));
    write_be32(loadable_pe, 0x320U, 0x48000011U); // bl import thunk at +0x10.
    write_be32(loadable_pe, 0x324U, (14U << 26U) | (7U << 21U) | (3U << 16U));
    write_be32(loadable_pe, 0x330U, 0x0100010eU); // thunk, library 0, ordinal 270.
    auto loadable_xex = make_xex2();
    write_be32(loadable_xex, 0x444U, 0x830a0U);
    write_be32(loadable_xex, 0x400U, 0x78U);
    write_be32(loadable_xex, 0x41cU, 0x5cU);
    write_be16(loadable_xex, 0x442U, 13U);
    write_be32(loadable_xex, 0x448U, 0x830a4U);
    write_be32(loadable_xex, 0x44cU, 0x830a8U);
    write_be32(loadable_xex, 0x450U, 0x830acU);
    write_be32(loadable_xex, 0x454U, 0x830b0U);
    write_be32(loadable_xex, 0x458U, 0x830b4U);
    write_be32(loadable_xex, 0x45cU, 0x830b8U);
    write_be32(loadable_xex, 0x460U, 0x830bcU);
    write_be32(loadable_xex, 0x464U, 0x830c0U);
    write_be32(loadable_xex, 0x468U, 0x830c4U);
    write_be32(loadable_xex, 0x46cU, 0x830c8U);
    write_be32(loadable_xex, 0x470U, 0x830fcU);
    write_be32(loadable_xex, 0x474U, 0x83130U);
    loadable_xex.insert(loadable_xex.end(), loadable_pe.begin(), loadable_pe.end());
    project360::memory::GuestMemory guest_memory(0x5000U);
    const std::array<std::byte, 8> kernel_destination_seed{
        std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef},
        std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef}};
    const std::array<std::byte, 5> ansi_source{
        std::byte{'G'}, std::byte{'A'}, std::byte{'M'}, std::byte{'E'}, std::byte{0}};
    const std::array<std::byte, 6> unicode_source{
        std::byte{0x03}, std::byte{0xa9}, std::byte{0x4e},
        std::byte{0x2d}, std::byte{0}, std::byte{0}};
    const std::array<std::byte, 8> integer_unicode_descriptor{
        std::byte{0}, std::byte{8}, std::byte{0}, std::byte{8},
        std::byte{0}, std::byte{0}, std::byte{0x50}, std::byte{0xc0}};
    const std::array<std::byte, 8> integer_unicode_source{
        std::byte{0}, std::byte{'0'}, std::byte{0}, std::byte{'x'},
        std::byte{0}, std::byte{'2'}, std::byte{0}, std::byte{'a'}};
    const std::array<std::byte, 8> integer_output_descriptor{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{16},
        std::byte{0}, std::byte{0}, std::byte{0x50}, std::byte{0xe0}};
    const std::array<std::byte, 8> compare_string_left_descriptor{
        std::byte{0}, std::byte{5}, std::byte{0}, std::byte{5},
        std::byte{0}, std::byte{0}, std::byte{0x51}, std::byte{0x60}};
    const std::array<std::byte, 8> compare_string_right_descriptor{
        std::byte{0}, std::byte{5}, std::byte{0}, std::byte{5},
        std::byte{0}, std::byte{0}, std::byte{0x51}, std::byte{0x70}};
    const std::array<std::byte, 5> compare_string_left{
        std::byte{'A'}, std::byte{'l'}, std::byte{'p'}, std::byte{'h'}, std::byte{'a'}};
    const std::array<std::byte, 5> compare_string_right{
        std::byte{'a'}, std::byte{'L'}, std::byte{'P'}, std::byte{'H'}, std::byte{'A'}};
    check(guest_memory.load(0x4000U, kernel_destination_seed),
          "integration fixture initializes a separate guest kernel destination");
    check(guest_memory.load(0x4040U, ansi_source),
          "integration fixture seeds the source for a guest ANSI_STRING");
    check(guest_memory.load(0x4060U, unicode_source),
          "integration fixture seeds the source for a guest UNICODE_STRING");
    check(guest_memory.load(0x40b0U, integer_unicode_descriptor) &&
              guest_memory.load(0x40c0U, integer_unicode_source) &&
              guest_memory.load(0x40d0U, integer_output_descriptor),
          "integration fixture seeds counted UTF-16 input and output descriptors");
    check(guest_memory.load(0x4140U, compare_string_left_descriptor) &&
              guest_memory.load(0x4150U, compare_string_right_descriptor) &&
              guest_memory.load(0x4160U, compare_string_left) &&
              guest_memory.load(0x4170U, compare_string_right),
          "integration fixture seeds counted ANSI strings with different ASCII case");
    const std::array<std::byte, 68> integer_output_seed{};
    check(guest_memory.load(0x40e0U, integer_output_seed),
          "integration fixture reserves the Unicode conversion output buffer");
    const auto loaded = project360::loader::load_xex2_uncompressed(loadable_xex,
                                                                    guest_memory, 0U);
    check(loaded && loaded.image->image_base == 0x82000U &&
              loaded.image->entry_point == 0x83000U && loaded.image->mapped_size == 0x2000U,
          "unencrypted uncompressed XEX2 maps decoded Xbox PE image pages");
    check(loaded && loaded.image->xex.import_libraries.size() == 1U &&
              loaded.image->xex.import_libraries[0].import_types.size() == 13U &&
              loaded.image->xex.import_libraries[0].import_types[0] == 1U &&
              loaded.image->xex.import_libraries[0].import_ordinals[0] == 284U &&
              loaded.image->xex.import_libraries[0].import_ordinals[1] == 285U &&
              loaded.image->xex.import_libraries[0].import_ordinals[2] == 269U &&
              loaded.image->xex.import_libraries[0].import_ordinals[3] == 268U &&
              loaded.image->xex.import_libraries[0].import_ordinals[4] == 298U &&
              loaded.image->xex.import_libraries[0].import_ordinals[5] == 320U &&
              loaded.image->xex.import_libraries[0].import_ordinals[6] == 289U &&
              loaded.image->xex.import_libraries[0].import_ordinals[7] == 290U &&
              loaded.image->xex.import_libraries[0].import_ordinals[8] == 307U &&
              loaded.image->xex.import_libraries[0].import_ordinals[9] == 318U &&
              loaded.image->xex.import_libraries[0].import_ordinals[10] == 309U &&
              loaded.image->xex.import_libraries[0].import_ordinals[11] == 293U &&
              loaded.image->xex.import_libraries[0].import_ordinals[12] == 270U,
          "loader decodes function-thunk records and Xbox kernel ordinals from PE bytes");
    std::uint32_t loaded_instruction = 0;
    check(guest_memory.fetch_u32_be(0x83000U, loaded_instruction) &&
              loaded_instruction == ((14U << 26U) | (3U << 21U) | 0x5000U),
          "loader copies executable PE section bytes to the mapped guest address");
    std::uint8_t loaded_byte = 0;
    check(guest_memory.read_u8(0x82000U, loaded_byte) && loaded_byte == 'M' &&
              !guest_memory.fetch_u32_be(0x82000U, loaded_instruction) &&
              !guest_memory.write_u8(0x83000U, 0U),
          "loader gives headers read-only pages and applies section execute/write permissions");
    check(guest_memory.map_page(0x5000U, 0x4000U,
                                project360::memory::GuestPageAccess::read |
                                    project360::memory::GuestPageAccess::write),
          "integration fixture maps writable guest memory for the kernel import");
    project360::cpu::CpuState guest_cpu{};
    guest_cpu.pc = loaded.image ? loaded.image->entry_point : 0U;
    if (loaded.image) {
        const project360::kernel::GuestExecutor executor(*loaded.image);
        const auto set_destination = executor.step(guest_cpu, guest_memory);
        const auto set_length = executor.step(guest_cpu, guest_memory);
        const auto set_fill = executor.step(guest_cpu, guest_memory);
        const auto call_import = executor.step(guest_cpu, guest_memory);
        const auto invoke_import = executor.step(guest_cpu, guest_memory);
        std::uint32_t filled_word = 0U;
        const bool fill_visible = guest_memory.read_u32_be(0x5000U, filled_word) &&
                                  filled_word == 0xababababU;
        const auto set_pattern = executor.step(guest_cpu, guest_memory);
        const auto call_ulong_import = executor.step(guest_cpu, guest_memory);
        const auto invoke_ulong_import = executor.step(guest_cpu, guest_memory);
        std::uint32_t ulong_pattern_first = 0U;
        std::uint32_t ulong_pattern_second = 0U;
        const bool ulong_pattern_visible =
            guest_memory.read_u32_be(0x5000U, ulong_pattern_first) &&
            guest_memory.read_u32_be(0x5004U, ulong_pattern_second) &&
            ulong_pattern_first == 0x00001234U && ulong_pattern_second == 0x00001234U;
        const auto call_compare_ulong_import = executor.step(guest_cpu, guest_memory);
        const auto invoke_compare_ulong_import = executor.step(guest_cpu, guest_memory);
        const auto save_compare_ulong_result = executor.step(guest_cpu, guest_memory);
        const auto compare_destination = executor.step(guest_cpu, guest_memory);
        const auto compare_source = executor.step(guest_cpu, guest_memory);
        const auto compare_length = executor.step(guest_cpu, guest_memory);
        const auto call_compare_import = executor.step(guest_cpu, guest_memory);
        const auto invoke_compare_import = executor.step(guest_cpu, guest_memory);
        const auto save_compare_result = executor.step(guest_cpu, guest_memory);
        const auto set_move_destination = executor.step(guest_cpu, guest_memory);
        const auto set_move_source = executor.step(guest_cpu, guest_memory);
        const auto set_move_length = executor.step(guest_cpu, guest_memory);
        const auto call_move_import = executor.step(guest_cpu, guest_memory);
        const auto invoke_move_import = executor.step(guest_cpu, guest_memory);
        std::uint32_t moved_word = 0U;
        const bool move_visible = guest_memory.read_u32_be(0x5008U, moved_word) &&
                                  moved_word == 0x00001234U;
        const auto set_zero_destination = executor.step(guest_cpu, guest_memory);
        const auto set_zero_length = executor.step(guest_cpu, guest_memory);
        const auto call_zero_import = executor.step(guest_cpu, guest_memory);
        const auto invoke_zero_import = executor.step(guest_cpu, guest_memory);
        std::uint32_t cleared_word = 1U;
        std::uint32_t cleared_copy_word = 1U;
        const auto resume_guest = executor.step(guest_cpu, guest_memory);
        const auto set_ansi_destination = executor.step(guest_cpu, guest_memory);
        const auto set_ansi_source = executor.step(guest_cpu, guest_memory);
        const auto call_ansi_import = executor.step(guest_cpu, guest_memory);
        const auto invoke_ansi_import = executor.step(guest_cpu, guest_memory);
        const auto read_ansi_header = executor.step(guest_cpu, guest_memory);
        const auto read_ansi_buffer = executor.step(guest_cpu, guest_memory);
        const auto set_unicode_destination = executor.step(guest_cpu, guest_memory);
        const auto set_unicode_source = executor.step(guest_cpu, guest_memory);
        const auto call_unicode_import = executor.step(guest_cpu, guest_memory);
        const auto invoke_unicode_import = executor.step(guest_cpu, guest_memory);
        const auto read_unicode_header = executor.step(guest_cpu, guest_memory);
        const auto read_unicode_buffer = executor.step(guest_cpu, guest_memory);
        const auto compare_ulong_result = guest_cpu.gpr[10];
        const auto compare_result = guest_cpu.gpr[9];
        const auto integer_call_run = executor.run(guest_cpu, guest_memory, 16U);
        const auto integer_to_unicode_run = executor.run(guest_cpu, guest_memory, 6U);
        project360::cpu::CpuState compare_string_cpu{};
        compare_string_cpu.pc = 0x83100U;
        const auto compare_string_run = executor.run(compare_string_cpu, guest_memory, 12U);
        std::uint32_t parsed_integer = 0U;
        std::uint16_t output_string_length = 0U;
        check(set_destination.status == project360::cpu::StepStatus::executed &&
                  set_length.status == project360::cpu::StepStatus::executed &&
                  set_fill.status == project360::cpu::StepStatus::executed &&
                  call_import.status == project360::cpu::StepStatus::executed &&
                  invoke_import.status == project360::cpu::StepStatus::executed &&
                  set_pattern.status == project360::cpu::StepStatus::executed &&
                  call_ulong_import.status == project360::cpu::StepStatus::executed &&
                  invoke_ulong_import.status == project360::cpu::StepStatus::executed &&
                  call_compare_ulong_import.status == project360::cpu::StepStatus::executed &&
                  invoke_compare_ulong_import.status == project360::cpu::StepStatus::executed &&
                  save_compare_ulong_result.status == project360::cpu::StepStatus::executed &&
                  compare_destination.status == project360::cpu::StepStatus::executed &&
                  compare_source.status == project360::cpu::StepStatus::executed &&
                  compare_length.status == project360::cpu::StepStatus::executed &&
                  call_compare_import.status == project360::cpu::StepStatus::executed &&
                  invoke_compare_import.status == project360::cpu::StepStatus::executed &&
                  save_compare_result.status == project360::cpu::StepStatus::executed &&
                  set_move_destination.status == project360::cpu::StepStatus::executed &&
                  set_move_source.status == project360::cpu::StepStatus::executed &&
                  set_move_length.status == project360::cpu::StepStatus::executed &&
                  call_move_import.status == project360::cpu::StepStatus::executed &&
                  invoke_move_import.status == project360::cpu::StepStatus::executed &&
                  set_zero_destination.status == project360::cpu::StepStatus::executed &&
                  set_zero_length.status == project360::cpu::StepStatus::executed &&
                  call_zero_import.status == project360::cpu::StepStatus::executed &&
                  invoke_zero_import.status == project360::cpu::StepStatus::executed &&
                  resume_guest.status == project360::cpu::StepStatus::executed &&
                  set_ansi_destination.status == project360::cpu::StepStatus::executed &&
                  set_ansi_source.status == project360::cpu::StepStatus::executed &&
                  call_ansi_import.status == project360::cpu::StepStatus::executed &&
                  invoke_ansi_import.status == project360::cpu::StepStatus::executed &&
                  read_ansi_header.status == project360::cpu::StepStatus::executed &&
                  read_ansi_buffer.status == project360::cpu::StepStatus::executed &&
                  set_unicode_destination.status == project360::cpu::StepStatus::executed &&
                  set_unicode_source.status == project360::cpu::StepStatus::executed &&
                  call_unicode_import.status == project360::cpu::StepStatus::executed &&
                  invoke_unicode_import.status == project360::cpu::StepStatus::executed &&
                  read_unicode_header.status == project360::cpu::StepStatus::executed &&
                  read_unicode_buffer.status == project360::cpu::StepStatus::executed &&
                  integer_call_run.instructions_executed == 16U &&
                  integer_call_run.stop.status ==
                      project360::cpu::StepStatus::instruction_limit_reached &&
                  integer_to_unicode_run.instructions_executed == 6U &&
                  integer_to_unicode_run.stop.status ==
                      project360::cpu::StepStatus::instruction_limit_reached,
              "loaded XEX program calls twelve imported kernel routines and resumes");
        check(compare_string_run.instructions_executed == 12U &&
                  compare_string_run.stop.status ==
                      project360::cpu::StepStatus::instruction_limit_reached &&
                  compare_string_cpu.pc == 0x83128U && compare_string_cpu.gpr[6] == 0U &&
                  compare_string_cpu.gpr[7] == std::numeric_limits<std::uint64_t>::max(),
              "synthetic XEX calls RtlCompareString through its import thunk in both ASCII case modes");
        check(fill_visible,
              "XEX RtlFillMemory thunk fills guest bytes using the Xenon argument registers");
        check(ulong_pattern_visible,
              "XEX RtlFillMemoryUlong thunk repeats the GPR5 word in big-endian guest memory");
        check(compare_ulong_result == 8U,
              "XEX RtlCompareMemoryUlong thunk returns the matching byte count in GPR3");
        check(compare_result == 4U,
              "XEX RtlCompareMemory thunk returns its matching byte count in GPR3");
        check(move_visible,
              "XEX RtlMoveMemory thunk copies the source bytes into guest destination memory");
        std::uint32_t ansi_header = 0U;
        std::uint32_t ansi_buffer = 0U;
        check(guest_memory.read_u32_be(0x5080U, ansi_header) &&
                  ansi_header == 0x00040005U &&
                  guest_memory.read_u32_be(0x5084U, ansi_buffer) && ansi_buffer == 0x5040U,
              "XEX RtlInitAnsiString thunk writes a big-endian header and source pointer");
        check(guest_cpu.gpr[7] == 0x00040006U && guest_cpu.gpr[8] == 0x5060U,
              "XEX RtlInitUnicodeString thunk writes byte lengths and source pointer");
        check(guest_cpu.gpr[3] == 0U && guest_cpu.gpr[11] == 0U &&
                  guest_cpu.gpr[12] == 0U && guest_cpu.pc == 0x830f8U &&
                  guest_memory.read_u32_be(0x50b8U, parsed_integer) && parsed_integer == 42U,
              "XEX integer conversion shims return through LR and guest code resumes");
        std::uint16_t upper_two = 0U;
        std::uint16_t upper_a = 0U;
        check(guest_memory.read_u16_be(0x50d0U, output_string_length) &&
                  output_string_length == 4U &&
                  guest_memory.read_u16_be(0x50e0U, upper_two) && upper_two == '2' &&
                  guest_memory.read_u16_be(0x50e2U, upper_a) && upper_a == 'A',
              "XEX RtlIntegerToUnicodeString thunk formats uppercase UTF-16 hexadecimal");
        check(guest_memory.read_u32_be(0x5000U, cleared_word) && cleared_word == 0U &&
                  guest_memory.read_u32_be(0x5008U, cleared_copy_word) &&
                  cleared_copy_word == 0U && guest_cpu.gpr[6] == 9U &&
                  guest_cpu.pc == 0x830f8U,
              "XEX RtlZeroMemory thunk clears bytes and returns through the caller LR");

        project360::cpu::CpuState bounded_cpu{};
        bounded_cpu.pc = loaded.image->entry_point;
        const auto zero_budget_run = executor.run(bounded_cpu, guest_memory, 0U);
        check(zero_budget_run.instructions_executed == 0U &&
                  zero_budget_run.stop.status ==
                      project360::cpu::StepStatus::instruction_limit_reached &&
                  bounded_cpu.pc == loaded.image->entry_point,
              "zero XEX instruction budget stops without changing guest state");
        const auto bounded_run = executor.run(bounded_cpu, guest_memory, 39U);
        check(bounded_run.instructions_executed == 39U &&
              bounded_run.stop.status ==
                      project360::cpu::StepStatus::instruction_limit_reached &&
                  bounded_cpu.pc == 0x8307cU && bounded_cpu.gpr[7] == 0x00040006U &&
                  bounded_cpu.gpr[8] == 0x5060U,
              "bounded XEX execution calls the string initializers and stops at its instruction budget");
        const auto byte_swap_run = executor.run(bounded_cpu, guest_memory, 8U);
        check(byte_swap_run.instructions_executed == 8U &&
                  byte_swap_run.stop.status ==
                      project360::cpu::StepStatus::instruction_limit_reached &&
                  bounded_cpu.gpr[9] == 0x34120000U && bounded_cpu.gpr[10] == 0x3412U,
              "bounded XEX execution calls both byte-swap imports and verifies their return values");

        project360::cpu::CpuState faulting_cpu{};
        faulting_cpu.pc = 0x830f8U;
        const auto faulting_run = executor.run(faulting_cpu, guest_memory, 2U);
        check(faulting_run.instructions_executed == 1U &&
                  faulting_run.stop.status == project360::cpu::StepStatus::fault &&
                  faulting_cpu.pc == 0x700U,
              "bounded XEX execution continues after a modeled exception and reports the subsequent vector-fetch fault");

        auto data_import_image = *loaded.image;
        data_import_image.xex.import_libraries[0].import_types[0] = 0U;
        const project360::kernel::GuestExecutor data_import_executor(data_import_image);
        project360::cpu::CpuState data_import_cpu{};
        data_import_cpu.pc = data_import_image.xex.import_libraries[0]
                                 .import_record_addresses[0];
        const auto data_import_result =
            data_import_executor.step(data_import_cpu, guest_memory);
        check(data_import_result.status == project360::cpu::StepStatus::fault &&
                  data_import_result.message.find("data import") != std::string::npos &&
                  data_import_cpu.pc ==
                      data_import_image.xex.import_libraries[0]
                          .import_record_addresses[0],
              "executing a data import faults explicitly without advancing the guest PC");
    } else {
        check(false, "loaded XEX image is available for kernel execution integration");
    }

    auto wrong_import_library = loadable_xex;
    write_be32(wrong_import_library, 0x1000U + 0x2a0U, 0x0101011cU);
    project360::memory::GuestMemory wrong_library_memory(0x4000U);
    const auto wrong_library_load = project360::loader::load_xex2_uncompressed(
        wrong_import_library, wrong_library_memory, 0U);
    check(!wrong_library_load && !wrong_library_memory.page_mapping_enabled(),
          "an import record naming a different library index is rejected before mapping");
    auto unsupported_import_type = loadable_xex;
    write_be32(unsupported_import_type, 0x1000U + 0x2a0U, 0x0200011cU);
    project360::memory::GuestMemory unsupported_type_memory(0x4000U);
    const auto unsupported_type_load = project360::loader::load_xex2_uncompressed(
        unsupported_import_type, unsupported_type_memory, 0U);
    check(!unsupported_type_load && !unsupported_type_memory.page_mapping_enabled(),
          "unknown XEX import record types are rejected before mapping");
    auto outside_import_address = loadable_xex;
    write_be32(outside_import_address, 0x444U, 0x84000U);
    project360::memory::GuestMemory outside_import_memory(0x4000U);
    const auto outside_import_load = project360::loader::load_xex2_uncompressed(
        outside_import_address, outside_import_memory, 0U);
    check(!outside_import_load && !outside_import_memory.page_mapping_enabled(),
          "an import record address outside the PE image is rejected before mapping");

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

    const auto compressed_xex = make_basic_compressed_xex(loadable_xex);
    project360::memory::GuestMemory compressed_memory(0x4000U);
    const auto compressed_result = project360::loader::load_xex2(
        compressed_xex, compressed_memory, 0U);
    std::uint32_t compressed_entry_instruction = 0;
    check(compressed_result && compressed_result.image->entry_point == 0x83000U &&
              compressed_memory.fetch_u32_be(0x83000U, compressed_entry_instruction) &&
              compressed_entry_instruction == ((14U << 26U) | (3U << 21U) | 0x5000U),
          "basic-compressed XEX blocks reconstruct and map a valid PE memory image");
    check(compressed_result &&
              compressed_result.image->xex.import_libraries[0].import_ordinals.back() == 270U,
          "basic-compressed XEX import thunks are decoded from guest image addresses");

    auto invalid_compressed_xex = compressed_xex;
    write_be32(invalid_compressed_xex, 0x388U, 0xffffffffU);
    project360::memory::GuestMemory invalid_compressed_memory(0x4000U);
    const auto invalid_compressed_result = project360::loader::load_xex2(
        invalid_compressed_xex, invalid_compressed_memory, 0U);
    check(!invalid_compressed_result && !invalid_compressed_memory.page_mapping_enabled(),
          "oversized basic-compression blocks fail before changing guest mappings");

    auto unsupported_compressed_xex = loadable_xex;
    write_be16(unsupported_compressed_xex, 0x386U, 2U);
    project360::memory::GuestMemory unsupported_compressed_memory(0x4000U);
    const auto unsupported_compressed_result = project360::loader::load_xex2(
        unsupported_compressed_xex, unsupported_compressed_memory, 0U);
    check(!unsupported_compressed_result && !unsupported_compressed_memory.page_mapping_enabled(),
          "unsupported normal-compressed XEX images fail before changing guest mappings");

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
