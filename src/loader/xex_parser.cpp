#include "loader/xex_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace project360::loader {
namespace {

constexpr std::uint32_t kXex1Magic = 0x58455831U;
constexpr std::uint32_t kXex2Magic = 0x58455832U;
constexpr std::uint32_t kXexpMagic = 0x58455825U;
constexpr std::uint32_t kExecutionInfoKey = 0x00040006U;
constexpr std::uint32_t kEntryPointKey = 0x00010100U;
constexpr std::size_t kFixedHeaderSize = 0x18U;
constexpr std::size_t kOptionalHeaderEntrySize = 8U;
constexpr std::size_t kExecutionInfoSize = 0x18U;
constexpr std::size_t kSecurityInfoMinimumSize = 0x184U;
constexpr std::uint32_t kMaximumHeaderSize = 16U * 1024U * 1024U;

bool read_be32(std::span<const std::byte> bytes, std::size_t offset, std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return false;
    value = (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
            (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 16U) |
            (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 8U) |
            std::to_integer<std::uint32_t>(bytes[offset + 3U]);
    return true;
}

bool range_fits(std::uint64_t offset, std::uint64_t length, std::uint64_t limit) {
    return offset <= limit && length <= limit - offset;
}

} // namespace

XexParseResult parse_xex2(std::span<const std::byte> file) {
    if (file.size() < kFixedHeaderSize) return {{}, "file is shorter than the XEX2 fixed header"};

    std::uint32_t magic = 0;
    std::uint32_t module_flags = 0;
    std::uint32_t header_size = 0;
    std::uint32_t security_offset = 0;
    std::uint32_t header_count = 0;
    if (!read_be32(file, 0U, magic) || !read_be32(file, 4U, module_flags) ||
        !read_be32(file, 8U, header_size) || !read_be32(file, 16U, security_offset) ||
        !read_be32(file, 20U, header_count)) {
        return {{}, "failed to read the XEX2 fixed header"};
    }
    if (magic == kXex1Magic || magic == kXexpMagic) {
        return {{}, "XEX1 and XEXP formats are not supported by this parser"};
    }
    if (magic != kXex2Magic) return {{}, "unrecognized XEX magic"};
    if (header_size < kFixedHeaderSize || header_size > kMaximumHeaderSize ||
        header_size > file.size()) {
        return {{}, "declared XEX header size is invalid or exceeds the supplied data"};
    }

    const auto optional_table_size = static_cast<std::uint64_t>(header_count) *
                                     kOptionalHeaderEntrySize;
    const auto optional_table_end = static_cast<std::uint64_t>(kFixedHeaderSize) +
                                    optional_table_size;
    if (!range_fits(kFixedHeaderSize, optional_table_size, header_size)) {
        return {{}, "optional-header directory exceeds the declared header size"};
    }
    if (security_offset < optional_table_end ||
        !range_fits(security_offset, 8U, header_size)) {
        return {{}, "security-info offset overlaps the directory or is out of bounds"};
    }

    std::uint32_t security_size = 0;
    std::uint32_t image_size = 0;
    if (!read_be32(file, security_offset, security_size) ||
        !read_be32(file, static_cast<std::size_t>(security_offset) + 4U, image_size) ||
        security_size < kSecurityInfoMinimumSize ||
        !range_fits(security_offset, security_size, header_size)) {
        return {{}, "XEX2 security-info block is truncated or has an invalid size"};
    }
    const auto page_count_offset = static_cast<std::size_t>(security_offset) + 0x180U;
    std::uint32_t page_descriptor_count = 0;
    if (!read_be32(file, page_count_offset, page_descriptor_count)) {
        return {{}, "XEX2 security-info page count is truncated"};
    }
    const auto descriptor_bytes = static_cast<std::uint64_t>(page_descriptor_count) * 0x18U;
    if (!range_fits(kSecurityInfoMinimumSize, descriptor_bytes, security_size)) {
        return {{}, "XEX2 page descriptor table exceeds the security-info block"};
    }

    XexMetadata metadata{};
    metadata.module_flags = module_flags;
    metadata.header_size = header_size;
    metadata.security_info_offset = security_offset;
    metadata.image_size = image_size;
    metadata.optional_header_count = header_count;
    bool found_execution_info = false;
    for (std::uint32_t i = 0; i < header_count; ++i) {
        const auto entry_offset = kFixedHeaderSize +
                                  static_cast<std::size_t>(i) * kOptionalHeaderEntrySize;
        std::uint32_t key = 0;
        std::uint32_t value = 0;
        if (!read_be32(file, entry_offset, key) || !read_be32(file, entry_offset + 4U, value)) {
            return {{}, "optional-header directory entry is truncated"};
        }
        if (key == kEntryPointKey) {
            if (metadata.entry_point) return {{}, "duplicate XEX entry-point header"};
            metadata.entry_point = value;
        } else if (key == kExecutionInfoKey) {
            if (found_execution_info) return {{}, "duplicate XEX execution-info header"};
            found_execution_info = true;
            if (!range_fits(value, kExecutionInfoSize, header_size) ||
                !range_fits(value, kExecutionInfoSize, file.size())) {
                return {{}, "XEX execution-info payload is out of bounds"};
            }
            std::uint32_t media_id = 0;
            std::uint32_t title_id = 0;
            if (!read_be32(file, value, media_id) || !read_be32(file, value + 12U, title_id)) {
                return {{}, "XEX execution-info payload is truncated"};
            }
            metadata.media_id = media_id;
            metadata.title_id = title_id;
        }
    }

    return {metadata, {}};
}

} // namespace project360::loader
