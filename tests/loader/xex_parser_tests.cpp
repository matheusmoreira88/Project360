#include "loader/xex_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
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

std::vector<std::byte> make_xex2() {
    std::vector<std::byte> bytes(0x1000U);
    write_be32(bytes, 0x00U, 0x58455832U);
    write_be32(bytes, 0x04U, 0x00000081U);
    write_be32(bytes, 0x08U, 0x1000U);
    write_be32(bytes, 0x10U, 0x100U);
    write_be32(bytes, 0x14U, 2U);
    write_be32(bytes, 0x18U, 0x00010100U); // Entry point (immediate).
    write_be32(bytes, 0x1cU, 0x82000U);
    write_be32(bytes, 0x20U, 0x00040006U); // Execution info (24-byte payload).
    write_be32(bytes, 0x24U, 0x300U);
    write_be32(bytes, 0x100U, 0x184U); // Security-info size.
    write_be32(bytes, 0x104U, 0x6000U); // Image size.
    write_be32(bytes, 0x280U, 0U);      // Page-descriptor count.
    write_be32(bytes, 0x300U, 0x12345678U); // Media ID.
    write_be32(bytes, 0x30cU, 0x545408A7U); // Title ID.
    return bytes;
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
              parsed.metadata->optional_header_count == 2U,
          "valid XEX2 headers expose bounded container metadata");
    check(parsed.metadata && parsed.metadata->title_id == 0x545408a7U &&
              parsed.metadata->media_id == 0x12345678U &&
              parsed.metadata->entry_point == 0x82000U,
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

    if (failures != 0) {
        std::cerr << failures << " XEX parser test(s) failed\n";
        return 1;
    }
    std::cout << "All XEX parser tests passed\n";
    return 0;
}
