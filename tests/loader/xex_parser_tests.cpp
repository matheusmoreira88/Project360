#include "loader/xex_parser.hpp"

#include "cpu/cpu_state.hpp"
#include "cpu/interpreter.hpp"
#include "kernel/guest_executor.hpp"
#include "kernel/kernel_dispatcher.hpp"
#include "kernel/guest_thread_context.hpp"
#include "memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
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
    write_be32(bytes, 0x14U, 6U);
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
    write_be32(bytes, 0x40U, 0x00020104U); // TLS info (16-byte payload).
    write_be32(bytes, 0x44U, 0x320U);
    write_be32(bytes, 0x100U, 0x184U); // Security-info size.
    write_be32(bytes, 0x104U, 0x6000U); // Image size.
    write_be32(bytes, 0x210U, 0x82000U); // Security-info load address.
    write_be32(bytes, 0x280U, 0U);      // Page-descriptor count.
    write_be32(bytes, 0x300U, 0x12345678U); // Media ID.
    write_be32(bytes, 0x304U, 0x00010005U); // Title version.
    write_be32(bytes, 0x308U, 0x00010002U); // Base version.
    write_be32(bytes, 0x30cU, 0x545408A7U); // Title ID.
    bytes[0x310U] = std::byte{2};           // Platform.
    bytes[0x311U] = std::byte{1};           // Executable type.
    bytes[0x312U] = std::byte{1};           // Disc number.
    bytes[0x313U] = std::byte{2};           // Disc count.
    write_be32(bytes, 0x314U, 0xaabbccddU); // Savegame ID.
    write_be32(bytes, 0x320U, 64U);         // TLS slot count.
    write_be32(bytes, 0x324U, 0x83040U);    // TLS raw-data address.
    write_be32(bytes, 0x328U, 0x20U);       // TLS data size.
    write_be32(bytes, 0x32cU, 0x10U);       // TLS raw-data size.
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

std::vector<std::byte> make_dbgprint_homebrew_xex() {
    constexpr std::uint32_t image_base = 0x82000U;
    constexpr std::uint32_t import_thunk = 0x83004U;
    constexpr std::uint32_t entry_point = 0x83100U;
    constexpr std::uint32_t format_address = 0x83120U;

    auto pe = make_xbox_pe32();
    write_le32(pe, 0x98U + 16U, entry_point - image_base);
    write_le32(pe, 0x178U + 8U, 0x200U);
    write_le32(pe, 0x178U + 16U, 0x200U);
    pe.resize(0x400U);
    write_be32(pe, 0x204U, 0x01000003U); // xboxkrnl.exe DbgPrint, ordinal 3.
    write_be32(pe, 0x300U, (15U << 26U) | (3U << 21U) | 8U); // lis r3, 8.
    write_be32(pe, 0x304U, (24U << 26U) | (3U << 21U) | (3U << 16U) |
                                (format_address & 0xffffU)); // ori r3,r3,0x3120.
    write_be32(pe, 0x308U, (14U << 26U) | (4U << 21U) | 1U); // li r4,1.
    constexpr auto branch_pc = entry_point + 0x0cU;
    constexpr auto branch_displacement =
        static_cast<std::int32_t>(import_thunk) - static_cast<std::int32_t>(branch_pc);
    write_be32(pe, 0x30cU,
               (18U << 26U) |
                   (static_cast<std::uint32_t>(branch_displacement) & 0x03fffffcU) | 1U);
    write_be32(pe, 0x310U, (14U << 26U) | (6U << 21U) | 42U); // li r6,42 after return.
    constexpr char debug_format[] = "Homebrew boot %u";
    for (std::size_t index = 0U; index < sizeof(debug_format); ++index) {
        pe[0x320U + index] = static_cast<std::byte>(debug_format[index]);
    }

    auto xex = make_xex2();
    write_be32(xex, 0x1cU, entry_point);
    xex.insert(xex.end(), pe.begin(), pe.end());
    return xex;
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

std::vector<std::byte> make_page_descriptor_xex(std::span<const std::byte> xex,
                                                std::uint32_t section_type) {
    std::vector<std::byte> described(xex.begin(), xex.end());
    write_be32(described, 0x100U, 0x19cU);
    write_be32(described, 0x104U, 0x2000U);
    write_be32(described, 0x20cU, 0x10000000U);
    write_be32(described, 0x280U, 1U);
    write_be32(described, 0x284U, (2U << 4U) | section_type);
    return described;
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
              parsed.metadata->optional_header_count == 6U &&
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
              parsed.metadata->title_version == 0x00010005U &&
              parsed.metadata->base_version == 0x00010002U &&
              parsed.metadata->platform == 2U && parsed.metadata->executable_type == 1U &&
              parsed.metadata->disc_number == 1U && parsed.metadata->disc_count == 2U &&
              parsed.metadata->savegame_id == 0xaabbccddU &&
              parsed.metadata->tls_info && parsed.metadata->tls_info->slot_count == 64U &&
              parsed.metadata->tls_info->raw_data_address == 0x83040U &&
              parsed.metadata->tls_info->data_size == 0x20U &&
              parsed.metadata->tls_info->raw_data_size == 0x10U &&
              parsed.metadata->entry_point == 0x83000U &&
              parsed.metadata->encryption_type == 0U &&
              parsed.metadata->compression_type == 0U,
          "execution-info identifiers, versions, platform and disc fields decode in bounded big-endian form");

    auto duplicate_tls = bytes;
    write_be32(duplicate_tls, 0x14U, 7U);
    write_be32(duplicate_tls, 0x48U, 0x00020104U);
    write_be32(duplicate_tls, 0x4cU, 0x320U);
    const auto duplicate_tls_result = parse_xex2(duplicate_tls);
    check(!duplicate_tls_result && duplicate_tls_result.error.find("duplicate XEX TLS") !=
                                       std::string::npos,
          "duplicate TLS optional headers are rejected");

    auto truncated_tls = bytes;
    write_be32(truncated_tls, 0x44U, 0x0ff8U);
    const auto truncated_tls_result = parse_xex2(truncated_tls);
    check(!truncated_tls_result && truncated_tls_result.error.find("TLS-info") !=
                                       std::string::npos,
          "TLS payloads that extend past the declared header are rejected");

    auto described_pages = bytes;
    write_be32(described_pages, 0x100U, 0x19cU);
    write_be32(described_pages, 0x20cU, 0x10000000U);
    write_be32(described_pages, 0x280U, 1U);
    write_be32(described_pages, 0x284U, (6U << 4U) | 1U);
    for (std::size_t i = 0; i < 20U; ++i) described_pages[0x288U + i] = std::byte{0xa5};
    const auto parsed_pages = parse_xex2(described_pages);
    check(parsed_pages && parsed_pages.metadata->image_flags == 0x10000000U &&
              parsed_pages.metadata->page_descriptors.size() == 1U &&
              parsed_pages.metadata->page_descriptors[0].page_count == 6U &&
              parsed_pages.metadata->page_descriptors[0].section_type == 1U &&
              parsed_pages.metadata->page_descriptors[0].digest[19] == std::byte{0xa5},
          "XEX security metadata exposes image flags and bounded page descriptors");

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
    write_be32(loadable_pe, 0x2a0U, 0x0100011cU); // RtlCompareString, ordinal 284.
    write_be32(loadable_pe, 0x2a4U, 0x01000126U); // RtlFillMemoryUlong, ordinal 294.
    write_be32(loadable_pe, 0x2a8U, 0x0100011bU); // RtlCompareMemoryUlong, ordinal 283.
    write_be32(loadable_pe, 0x2acU, 0x0100011aU); // RtlCompareMemory, ordinal 282.
    write_be32(loadable_pe, 0x2b0U, 0x0100011eU); // RtlCompareUnicodeString, ordinal 286.
    write_be32(loadable_pe, 0x2b4U, 0x01000135U); // RtlNtStatusToDosError, ordinal 309.
    write_be32(loadable_pe, 0x2b8U, 0x0100012cU); // RtlInitAnsiString, ordinal 300.
    write_be32(loadable_pe, 0x2bcU, 0x0100012dU); // RtlInitUnicodeString, ordinal 301.
    write_be32(loadable_pe, 0x2c0U, 0x01000124U); // RtlDowncaseUnicodeChar, ordinal 292.
    write_be32(loadable_pe, 0x2c4U, 0x01000149U); // RtlUpcaseUnicodeChar, ordinal 329.
    write_be32(loadable_pe, 0x2c8U, 0x01000082U); // KeQueryInterruptTime, ordinal 130.
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
    write_be32(loadable_pe, 0x2fcU, 0x01000083U); // KeQueryPerformanceFrequency, ordinal 131.
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
    write_be32(loadable_pe, 0x330U, 0x01000084U); // KeQuerySystemTime, ordinal 132.
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
              loaded.image->xex.import_libraries[0].import_ordinals ==
                  std::vector<std::optional<std::uint16_t>>{
                      284U, 294U, 283U, 282U, 286U, 309U, 300U,
                      301U, 292U, 329U, 130U, 131U, 132U},
          "loader decodes supported Xbox 360 export ordinals from PE import records");
    if (loaded) {
        auto pool_image = *loaded.image;
        auto& pool_imports = pool_image.xex.import_libraries[0];
        pool_imports.import_record_addresses.push_back(0x83ff0U);
        pool_imports.import_types.emplace_back(1U);
        pool_imports.import_ordinals.emplace_back(
            project360::kernel::KernelDispatcher::ex_allocate_pool_with_tag_ordinal);
        pool_imports.import_record_addresses.push_back(0x83ff4U);
        pool_imports.import_types.emplace_back(1U);
        pool_imports.import_ordinals.emplace_back(
            project360::kernel::KernelDispatcher::ex_free_pool_ordinal);

        project360::memory::GuestMemory pool_guest_memory(4U * 1024U * 1024U);
        const auto pool_bootstrap_mapping = pool_guest_memory.map_page(
            0x70000000U, 0U, project360::memory::GuestPageAccess::read |
                               project360::memory::GuestPageAccess::write);
        const project360::kernel::GuestExecutor pool_executor(
            pool_image, {}, {0x71000000U, 8192U});
        project360::cpu::CpuState pool_state{};
        pool_state.pc = 0x83ff0U;
        pool_state.lr = 0x90001004U;
        pool_state.gpr[3] = 0U;
        pool_state.gpr[4] = 48U;
        pool_state.gpr[5] = 0x54455354U;
        const auto pool_allocate = pool_executor.step(pool_state, pool_guest_memory);
        const auto pool_pointer = static_cast<std::uint32_t>(pool_state.gpr[3]);
        pool_state.pc = 0x83ff4U;
        pool_state.lr = 0x90001008U;
        const auto pool_free = pool_executor.step(pool_state, pool_guest_memory);
        check(pool_bootstrap_mapping &&
                  pool_allocate.status == project360::cpu::StepStatus::executed &&
                  pool_pointer == 0x71000000U &&
                  pool_guest_memory.write_u32_be(pool_pointer, 0x600dbeefU) &&
                  pool_free.status == project360::cpu::StepStatus::executed &&
                  pool_state.pc == 0x90001008U,
              "XEX function-import thunks allocate writable guest pool memory and free it through LR");
    }
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
          "integration fixture maps a writable guest page for imported services");
    if (loaded.image) {
        const project360::kernel::GuestExecutor executor(*loaded.image);
        const auto& import_records = loaded.image->xex.import_libraries[0]
                                         .import_record_addresses;
        struct ImportCallResult {
            project360::cpu::StepResult step;
            project360::cpu::CpuState state;
        };
        const auto invoke_import = [&](std::size_t index,
                                       const std::array<std::uint64_t, 3>& arguments,
                                       project360::cpu::CpuState& guest_state) {
            guest_state = {};
            guest_state.pc = import_records[index];
            guest_state.lr = 0x90001004U;
            guest_state.gpr[3] = arguments[0];
            guest_state.gpr[4] = arguments[1];
            guest_state.gpr[5] = arguments[2];
            const auto step = executor.step(guest_state, guest_memory);
            return ImportCallResult{step, guest_state};
        };
        project360::cpu::CpuState imported_state{};
        const auto fill_ulong = invoke_import(1U, {0x5000U, 4U, 0x11223344U},
                                              imported_state);
        std::uint32_t imported_word = 0U;
        const auto compare_ulong = invoke_import(2U, {0x5000U, 4U, 0x11223344U},
                                                 imported_state);
        const auto compare_memory = invoke_import(3U, {0x5000U, 0x5000U, 4U},
                                                  imported_state);
        const auto compare_string = invoke_import(0U, {0x5140U, 0x5150U, 1U},
                                                  imported_state);
        const auto compare_unicode = invoke_import(4U, {0x50b0U, 0x50b0U, 0U},
                                                   imported_state);
        const auto translate_status = invoke_import(5U, {0xc0000005U, 0U, 0U},
                                                    imported_state);
        const auto init_ansi = invoke_import(6U, {0x5080U, 0x5040U, 0U},
                                             imported_state);
        std::uint32_t ansi_descriptor = 0U;
        const auto init_unicode = invoke_import(7U, {0x50a0U, 0x5060U, 0U},
                                                imported_state);
        std::uint32_t unicode_descriptor = 0U;
        const auto downcase = invoke_import(8U, {0x00c9U, 0U, 0U}, imported_state);
        const auto upcase = invoke_import(9U, {0x00e9U, 0U, 0U}, imported_state);
        const auto interrupt_time = invoke_import(10U, {0U, 0U, 0U}, imported_state);
        const auto performance_frequency = invoke_import(11U, {0U, 0U, 0U},
                                                         imported_state);
        const auto system_time = invoke_import(12U, {0x50f0U, 0U, 0U}, imported_state);
        std::uint64_t guest_system_time = 0U;
        check(fill_ulong.step.status == project360::cpu::StepStatus::executed &&
                  guest_memory.read_u32_be(0x5000U, imported_word) &&
                  imported_word == 0x11223344U,
              "XEX RtlFillMemoryUlong thunk writes the big-endian word pattern");
        check(compare_ulong.step.status == project360::cpu::StepStatus::executed &&
                  compare_ulong.state.gpr[3] == 4U,
              "XEX RtlCompareMemoryUlong thunk returns its matching byte count");
        check(compare_memory.step.status == project360::cpu::StepStatus::executed &&
                  compare_memory.state.gpr[3] == 4U,
              "XEX RtlCompareMemory thunk returns its matching byte count");
        check(compare_string.step.status == project360::cpu::StepStatus::executed &&
                  compare_string.state.gpr[3] == 0U,
              "XEX RtlCompareString thunk handles ASCII case-insensitive equality");
        check(compare_unicode.step.status == project360::cpu::StepStatus::executed &&
                  compare_unicode.state.gpr[3] == 0U,
              "XEX RtlCompareUnicodeString thunk handles a valid counted UTF-16 descriptor");
        check(translate_status.step.status == project360::cpu::StepStatus::executed &&
                  translate_status.state.gpr[3] == 998U,
              "XEX RtlNtStatusToDosError thunk translates a supported access status");
        check(init_ansi.step.status == project360::cpu::StepStatus::executed &&
                  guest_memory.read_u32_be(0x5080U, ansi_descriptor) &&
                  ansi_descriptor == 0x00040005U,
              "XEX RtlInitAnsiString thunk writes the counted-string header");
        check(init_unicode.step.status == project360::cpu::StepStatus::executed &&
                  guest_memory.read_u32_be(0x50a0U, unicode_descriptor) &&
                  unicode_descriptor == 0x00040006U,
              "XEX RtlInitUnicodeString thunk writes byte lengths");
        check(downcase.step.status == project360::cpu::StepStatus::executed &&
                  downcase.state.gpr[3] == 0x00e9U,
              "XEX RtlDowncaseUnicodeChar thunk maps a supported Latin-1 character");
        check(upcase.step.status == project360::cpu::StepStatus::executed &&
                  upcase.state.gpr[3] == 0x00c9U,
              "XEX RtlUpcaseUnicodeChar thunk maps a supported Latin-1 character");
        check(interrupt_time.step.status == project360::cpu::StepStatus::executed,
              "XEX KeQueryInterruptTime thunk returns through its import record");
        check(performance_frequency.step.status == project360::cpu::StepStatus::executed &&
                  performance_frequency.state.gpr[3] ==
                      project360::kernel::KernelDispatcher::xbox_360_performance_frequency,
              "XEX KeQueryPerformanceFrequency thunk returns the modeled counter rate");
        check(system_time.step.status == project360::cpu::StepStatus::executed &&
                  guest_memory.read_u64_be(0x50f0U, guest_system_time) &&
                  guest_system_time != 0U,
              "XEX KeQuerySystemTime thunk writes a guest big-endian timestamp");

        project360::cpu::CpuState bounded_cpu{};
        bounded_cpu.pc = loaded.image->entry_point;
        const auto zero_budget_run = executor.run(bounded_cpu, guest_memory, 0U);
        check(zero_budget_run.instructions_executed == 0U &&
                  zero_budget_run.stop.status ==
                      project360::cpu::StepStatus::instruction_limit_reached &&
                  bounded_cpu.pc == loaded.image->entry_point,
              "zero XEX instruction budget stops without changing guest state");
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

    {
        auto debug_xex = make_dbgprint_homebrew_xex();
        bool debug_xex_written = false;
        {
            std::ofstream output(test_path, std::ios::binary | std::ios::trunc);
            if (output) {
                output.write(reinterpret_cast<const char*>(debug_xex.data()),
                             static_cast<std::streamsize>(debug_xex.size()));
                debug_xex_written = static_cast<bool>(output);
            }
        }
        project360::memory::GuestMemory debug_memory(4U * 1024U * 1024U);
        const auto debug_loaded = debug_xex_written
                                      ? project360::loader::load_xex2_file(
                                            test_path, debug_memory, 0U)
                                      : project360::loader::XexLoadResult{};
        check(debug_xex_written && debug_loaded &&
                  debug_loaded.image->entry_point == 0x83100U &&
                  debug_loaded.image->xex.import_libraries.size() == 1U &&
                  debug_loaded.image->xex.import_libraries[0].import_ordinals ==
                      std::vector<std::optional<std::uint16_t>>{3U},
              "file loader reads a minimal synthetic homebrew XEX and decodes DbgPrint");

        if (debug_loaded) {
            project360::cpu::CpuState debug_state{};
            const auto debug_thread =
                project360::kernel::initialize_main_thread_context(
                    *debug_loaded.image, debug_memory, debug_state);
            check(debug_thread && debug_state.pc == 0x83100U && debug_state.gpr[1] != 0U &&
                      debug_state.gpr[13] == debug_thread.context->pcr_address,
                  "minimal homebrew XEX receives the bounded main-thread launch context");
            if (debug_thread) {
                const project360::kernel::GuestExecutor debug_executor(
                    *debug_loaded.image,
                    {debug_thread.context->tls_slot_address,
                     debug_thread.context->tls_slot_count});
                std::ostringstream debug_log;
                auto* const original_log_buffer = std::clog.rdbuf(debug_log.rdbuf());
                const auto debug_run = debug_executor.run(debug_state, debug_memory, 6U);
                std::clog.rdbuf(original_log_buffer);
                check(debug_run.instructions_executed == 6U &&
                          debug_run.stop.status ==
                              project360::cpu::StepStatus::instruction_limit_reached &&
                          debug_state.pc == 0x83114U && debug_state.gpr[6] == 42U &&
                          debug_log.str().find("[guest] Homebrew boot 1") !=
                              std::string::npos,
                      "loaded homebrew XEX initializes a thread, calls DbgPrint and resumes guest code");
            }
        }
        std::filesystem::remove(test_path, remove_error);
    }

    {
        constexpr std::uint32_t program_base = 0x1000U;
        constexpr std::uint32_t import_thunk = 0x1100U;
        constexpr std::uint32_t destination = 0x2000U;
        project360::loader::LoadedXexImage guest_program{};
        guest_program.entry_point = program_base;
        project360::loader::XexImportLibrary kernel_imports{};
        kernel_imports.name = "xboxkrnl.exe";
        kernel_imports.import_record_addresses = {import_thunk};
        kernel_imports.import_types = {1U};
        kernel_imports.import_ordinals = {
            project360::kernel::KernelDispatcher::rtl_fill_memory_ulong_ordinal};
        guest_program.xex.import_libraries.push_back(std::move(kernel_imports));

        project360::memory::GuestMemory program_memory(0x4000U);
        const std::array<std::uint32_t, 6> program_words{
            (14U << 26U) | (3U << 21U) | destination, // li r3, destination
            (14U << 26U) | (4U << 21U) | 4U,          // li r4, 4 bytes
            (15U << 26U) | (5U << 21U) | 0x1122U,     // lis r5, 0x1122
            (24U << 26U) | (5U << 21U) | (5U << 16U) | 0x3344U, // ori r5,r5,0x3344
            0x480000f1U,                              // bl 0x1100
            (14U << 26U) | (6U << 21U) | 0x55aaU,    // li r6, 0x55aa after return
        };
        std::array<std::byte, program_words.size() * sizeof(std::uint32_t)> program_bytes{};
        for (std::size_t index = 0; index < program_words.size(); ++index) {
            const auto word = program_words[index];
            const auto offset = index * sizeof(std::uint32_t);
            program_bytes[offset] = static_cast<std::byte>(word >> 24U);
            program_bytes[offset + 1U] = static_cast<std::byte>(word >> 16U);
            program_bytes[offset + 2U] = static_cast<std::byte>(word >> 8U);
            program_bytes[offset + 3U] = static_cast<std::byte>(word);
        }
        const bool program_loaded = program_memory.load(program_base, program_bytes);
        project360::kernel::GuestExecutor guest_program_executor(guest_program);
        project360::cpu::CpuState guest_program_state{};
        guest_program_state.pc = guest_program.entry_point;
        const auto guest_program_run = guest_program_executor.run(
            guest_program_state, program_memory, program_words.size() + 1U);
        std::uint32_t filled_word = 0U;
        check(program_loaded &&
                  guest_program_run.instructions_executed == program_words.size() + 1U &&
                  guest_program_run.stop.status ==
                      project360::cpu::StepStatus::instruction_limit_reached &&
                  guest_program_state.pc == program_base + program_words.size() * 4U &&
                  guest_program_state.gpr[6] == 0x55aaU &&
                  program_memory.read_u32_be(destination, filled_word) &&
                  filled_word == 0x11223344U,
              "synthetic PowerPC homebrew calls a verified kernel import, writes guest memory, and resumes after return");
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
              compressed_result.image->xex.import_libraries[0].import_ordinals.back() == 132U,
          "basic-compressed XEX import thunks are decoded from guest image addresses");

    auto thread_xex = loadable_xex;
    thread_xex.resize(thread_xex.size() + 0x10U, std::byte{0});
    write_be32(thread_xex, 0x324U, 0x83140U); // Separate TLS template at PE RVA 0x1140.
    write_le32(thread_xex, 0x1000U + 0x178U + 8U, 0x150U);
    write_le32(thread_xex, 0x1000U + 0x178U + 16U, 0x150U);
    thread_xex[0x1340U] = std::byte{0xa5};
    thread_xex[0x1341U] = std::byte{0x5a};
    project360::memory::GuestMemory thread_memory(0x40000U);
    const auto thread_loaded = project360::loader::load_xex2(
        thread_xex, thread_memory, 0U);
    std::uint8_t template_byte = 0U;
    const bool template_read = thread_loaded &&
                               thread_memory.read_u8(0x83140U, template_byte);
    project360::cpu::CpuState launch_state{};
    const auto thread_init = thread_loaded
                                 ? project360::kernel::initialize_main_thread_context(
                                       *thread_loaded.image, thread_memory, launch_state)
                                 : project360::kernel::GuestThreadInitResult{};
    std::uint8_t copied_template_byte = 0U;
    std::uint8_t zero_tail_byte = 0xffU;
    std::uint32_t pcr_tls = 0U;
    std::uint32_t pcr_self = 0U;
    std::uint32_t pcr_stack_base = 0U;
    std::uint32_t pcr_stack_limit = 0U;
    std::uint32_t pcr_current_thread = 0U;
    std::uint32_t kthread_stack_base = 0U;
    std::uint32_t kthread_stack_limit = 0U;
    std::uint32_t kthread_tls = 0U;
    std::uint32_t kthread_id = 0U;
    std::uint32_t kthread_start = 0U;
    std::uint32_t pcr_dpc_active = 1U;
    std::uint8_t pcr_cpu = 0xffU;
    std::uint8_t kthread_type = 0U;
    check(thread_init && template_read &&
              thread_memory.read_u8(thread_init.context->tls_address, copied_template_byte) &&
              template_byte == 0xa5U && copied_template_byte == template_byte &&
              thread_memory.read_u8(thread_init.context->tls_address + 0x10U,
                                    zero_tail_byte) && zero_tail_byte == 0U &&
              thread_memory.read_u32_be(thread_init.context->pcr_address, pcr_tls) &&
              thread_memory.read_u32_be(thread_init.context->pcr_address + 0x30U, pcr_self) &&
              thread_memory.read_u32_be(thread_init.context->pcr_address + 0x70U,
                                        pcr_stack_base) &&
              thread_memory.read_u32_be(thread_init.context->pcr_address + 0x74U,
                                        pcr_stack_limit) &&
              thread_memory.read_u32_be(thread_init.context->pcr_address + 0x100U,
                                        pcr_current_thread) &&
              thread_memory.read_u8(thread_init.context->pcr_address + 0x10cU, pcr_cpu) &&
              thread_memory.read_u32_be(thread_init.context->pcr_address + 0x150U,
                                        pcr_dpc_active) &&
              thread_memory.read_u8(thread_init.context->thread_object_address,
                                    kthread_type) &&
              thread_memory.read_u32_be(thread_init.context->thread_object_address + 0x5cU,
                                        kthread_stack_base) &&
              thread_memory.read_u32_be(thread_init.context->thread_object_address + 0x60U,
                                        kthread_stack_limit) &&
              thread_memory.read_u32_be(thread_init.context->thread_object_address + 0x68U,
                                        kthread_tls) &&
              thread_memory.read_u32_be(thread_init.context->thread_object_address + 0x14cU,
                                        kthread_id) &&
              thread_memory.read_u32_be(thread_init.context->thread_object_address + 0x150U,
                                        kthread_start) &&
              pcr_tls == thread_init.context->tls_address &&
              pcr_self == thread_init.context->pcr_address &&
              pcr_current_thread == thread_init.context->thread_object_address &&
              pcr_stack_base == thread_init.context->stack_base &&
              pcr_stack_limit == thread_init.context->stack_limit &&
              kthread_type == 6U &&
              kthread_stack_base == thread_init.context->stack_base &&
              kthread_stack_limit == thread_init.context->stack_limit &&
              kthread_tls == thread_init.context->tls_address && kthread_id == 1U &&
              kthread_start == thread_loaded.image->entry_point &&
              pcr_cpu == 0U && pcr_dpc_active == 0U &&
              launch_state.pc == thread_loaded.image->entry_point &&
              launch_state.gpr[13] == thread_init.context->pcr_address &&
              launch_state.gpr[1] == thread_init.context->stack_base &&
              !thread_memory.page_mapped(thread_init.context->stack_limit - 0x1000U),
          "XEX main-thread setup copies static TLS, builds PCR and stack, and leaves a guard page");

    project360::memory::GuestMemory constrained_thread_memory(0x4000U);
    const auto constrained_loaded = project360::loader::load_xex2(
        loadable_xex, constrained_thread_memory, 0U);
    project360::cpu::CpuState unchanged_launch_state{};
    const auto constrained_init = constrained_loaded
                                      ? project360::kernel::initialize_main_thread_context(
                                            *constrained_loaded.image,
                                            constrained_thread_memory,
                                            unchanged_launch_state)
                                      : project360::kernel::GuestThreadInitResult{};
    check(!constrained_init && !constrained_init.error.empty() &&
              unchanged_launch_state.pc == 0U && unchanged_launch_state.gpr[1] == 0U &&
              unchanged_launch_state.gpr[13] == 0U &&
              constrained_thread_memory.page_mapped(0x83000U),
          "main-thread setup reports exhausted backing without changing CPU launch state");

    const auto code_pages_xex = make_page_descriptor_xex(loadable_xex, 1U);
    project360::memory::GuestMemory code_pages_memory(0x4000U);
    const auto code_pages_result = project360::loader::load_xex2(
        code_pages_xex, code_pages_memory, 0U);
    std::uint32_t descriptor_entry_instruction = 0;
    check(code_pages_result && code_pages_result.image->mapped_size == 0x2000U &&
              code_pages_memory.fetch_u32_be(0x83000U, descriptor_entry_instruction) &&
              descriptor_entry_instruction == ((14U << 26U) | (3U << 21U) | 0x5000U) &&
              !code_pages_memory.write_u8(0x83000U, 0U),
          "XEX code page descriptors preserve executable read-only guest page access");

    const auto readonly_pages_xex = make_page_descriptor_xex(loadable_xex, 3U);
    project360::memory::GuestMemory readonly_pages_memory(0x4000U);
    const auto readonly_pages_result = project360::loader::load_xex2(
        readonly_pages_xex, readonly_pages_memory, 0U);
    check(!readonly_pages_result && !readonly_pages_memory.page_mapping_enabled(),
          "XEX loader rejects an entry point in a page descriptor marked read-only");

    auto oversized_tls_xex = loadable_xex;
    write_be32(oversized_tls_xex, 0x320U, 0xffffffffU);
    project360::memory::GuestMemory oversized_tls_memory(0x4000U);
    const auto oversized_tls_result = project360::loader::load_xex2(
        oversized_tls_xex, oversized_tls_memory, 0U);
    check(!oversized_tls_result &&
              oversized_tls_result.error.find("TLS slot or template") != std::string::npos &&
              !oversized_tls_memory.page_mapping_enabled(),
          "XEX loader rejects oversized TLS allocations before changing guest mappings");

    auto outside_tls_xex = loadable_xex;
    write_be32(outside_tls_xex, 0x324U, 0x84000U);
    project360::memory::GuestMemory outside_tls_memory(0x4000U);
    const auto outside_tls_result = project360::loader::load_xex2(
        outside_tls_xex, outside_tls_memory, 0U);
    check(!outside_tls_result &&
              outside_tls_result.error.find("TLS raw-data template") != std::string::npos &&
              !outside_tls_memory.page_mapping_enabled(),
          "XEX loader rejects TLS templates outside the PE image before mapping");

    auto inverted_tls_xex = loadable_xex;
    write_be32(inverted_tls_xex, 0x328U, 4U);
    write_be32(inverted_tls_xex, 0x32cU, 8U);
    project360::memory::GuestMemory inverted_tls_memory(0x4000U);
    const auto inverted_tls_result = project360::loader::load_xex2(
        inverted_tls_xex, inverted_tls_memory, 0U);
    check(!inverted_tls_result &&
              inverted_tls_result.error.find("TLS slot or template") != std::string::npos &&
              !inverted_tls_memory.page_mapping_enabled(),
          "XEX loader rejects initialized TLS bytes beyond the declared TLS data area");

    const auto invalid_type_pages_xex = make_page_descriptor_xex(loadable_xex, 4U);
    project360::memory::GuestMemory invalid_type_pages_memory(0x4000U);
    const auto invalid_type_pages_result = project360::loader::load_xex2(
        invalid_type_pages_xex, invalid_type_pages_memory, 0U);
    check(!invalid_type_pages_result && !invalid_type_pages_memory.page_mapping_enabled(),
          "XEX loader rejects unknown page descriptor types before mapping");

    auto unaligned_pages_xex = make_page_descriptor_xex(loadable_xex, 1U);
    write_be32(unaligned_pages_xex, 0x20cU, 0U);
    project360::memory::GuestMemory unaligned_pages_memory(0x4000U);
    const auto unaligned_pages_result = project360::loader::load_xex2(
        unaligned_pages_xex, unaligned_pages_memory, 0U);
    check(!unaligned_pages_result && !unaligned_pages_memory.page_mapping_enabled(),
          "64 KiB XEX page mode rejects a misaligned image base before mapping");

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
