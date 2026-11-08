#include "loader/xex_parser.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

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

bool read_le16(std::span<const std::byte> bytes, std::size_t offset, std::uint16_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) return false;
    value = static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset]) |
                                       (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
    return true;
}

bool read_le32(std::span<const std::byte> bytes, std::size_t offset, std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return false;
    value = std::to_integer<std::uint32_t>(bytes[offset]) |
            (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
            (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
            (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
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

XexParseResult parse_xex2_file(const std::filesystem::path& path) {
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) return {{}, "unable to read XEX file size: " + size_error.message()};
    if (file_size < kFixedHeaderSize) return {{}, "file is shorter than the XEX2 fixed header"};

    std::ifstream input(path, std::ios::binary);
    if (!input) return {{}, "unable to open XEX file"};
    std::array<std::byte, kFixedHeaderSize> fixed_header{};
    input.read(reinterpret_cast<char*>(fixed_header.data()),
               static_cast<std::streamsize>(fixed_header.size()));
    if (input.gcount() != static_cast<std::streamsize>(fixed_header.size())) {
        return {{}, "unable to read the complete XEX2 fixed header"};
    }

    std::uint32_t header_size = 0;
    if (!read_be32(fixed_header, 8U, header_size) || header_size < kFixedHeaderSize ||
        header_size > kMaximumHeaderSize || header_size > file_size) {
        return {{}, "declared XEX header size is invalid or exceeds the file"};
    }

    std::vector<std::byte> header(header_size);
    input.clear();
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        return {{}, "unable to read the complete declared XEX header"};
    }
    return parse_xex2(header);
}

PeParseResult parse_xbox_pe32(std::span<const std::byte> image) {
    constexpr std::uint16_t kDosSignature = 0x5a4dU;
    constexpr std::uint32_t kNtSignature = 0x00004550U;
    constexpr std::uint16_t kPowerPcBeMachine = 0x01f2U;
    constexpr std::uint16_t kPe32Magic = 0x010bU;
    constexpr std::uint16_t kXboxSubsystem = 14U;
    constexpr std::uint16_t kImage32BitMachine = 0x0100U;
    constexpr std::uint32_t kSectionExecutable = 0x20000000U;
    constexpr std::size_t kDosHeaderSize = 0x40U;
    constexpr std::size_t kFileHeaderSize = 20U;
    constexpr std::size_t kPe32OptionalHeaderSize = 224U;
    constexpr std::size_t kSectionHeaderSize = 40U;
    constexpr std::uint16_t kMaximumSections = 96U;

    std::uint16_t dos_signature = 0;
    std::uint32_t nt_offset = 0;
    if (!read_le16(image, 0U, dos_signature) || dos_signature != kDosSignature ||
        image.size() < kDosHeaderSize || !read_le32(image, 0x3cU, nt_offset)) {
        return {{}, "decoded image has an invalid or truncated DOS header"};
    }
    if (!range_fits(nt_offset, 4U + kFileHeaderSize, image.size())) {
        return {{}, "PE header offset is outside the decoded image"};
    }

    std::uint32_t nt_signature = 0;
    if (!read_le32(image, nt_offset, nt_signature) || nt_signature != kNtSignature) {
        return {{}, "decoded image is missing the PE signature"};
    }
    const auto file_header = static_cast<std::size_t>(nt_offset) + 4U;
    std::uint16_t machine = 0;
    std::uint16_t section_count = 0;
    std::uint16_t optional_size = 0;
    std::uint16_t characteristics = 0;
    if (!read_le16(image, file_header, machine) ||
        !read_le16(image, file_header + 2U, section_count) ||
        !read_le16(image, file_header + 16U, optional_size) ||
        !read_le16(image, file_header + 18U, characteristics)) {
        return {{}, "PE file header is truncated"};
    }
    if (machine != kPowerPcBeMachine || (characteristics & kImage32BitMachine) == 0U) {
        return {{}, "PE machine type is not Xbox 360 PowerPC big-endian"};
    }
    if (section_count == 0U || section_count > kMaximumSections ||
        optional_size != kPe32OptionalHeaderSize) {
        return {{}, "PE section count or PE32 optional-header size is unsupported"};
    }

    const auto optional_header = file_header + kFileHeaderSize;
    if (!range_fits(optional_header, optional_size, image.size())) {
        return {{}, "PE optional header exceeds the decoded image"};
    }
    std::uint16_t optional_magic = 0;
    std::uint16_t subsystem = 0;
    XboxPeMetadata metadata{};
    if (!read_le16(image, optional_header, optional_magic) || optional_magic != kPe32Magic ||
        !read_le32(image, optional_header + 16U, metadata.entry_point_rva) ||
        !read_le32(image, optional_header + 28U, metadata.image_base) ||
        !read_le32(image, optional_header + 32U, metadata.section_alignment) ||
        !read_le32(image, optional_header + 36U, metadata.file_alignment) ||
        !read_le32(image, optional_header + 56U, metadata.image_size) ||
        !read_le32(image, optional_header + 60U, metadata.headers_size) ||
        !read_le16(image, optional_header + 68U, subsystem)) {
        return {{}, "PE32 optional header is truncated"};
    }
    if (subsystem != kXboxSubsystem || metadata.section_alignment == 0U ||
        metadata.file_alignment == 0U || metadata.image_size == 0U ||
        metadata.headers_size > image.size() || metadata.headers_size > metadata.image_size ||
        metadata.entry_point_rva >= metadata.image_size) {
        return {{}, "PE32 Xbox subsystem or image layout fields are invalid"};
    }

    const auto section_table = optional_header + optional_size;
    const auto section_table_size = static_cast<std::uint64_t>(section_count) * kSectionHeaderSize;
    if (!range_fits(section_table, section_table_size, image.size()) ||
        !range_fits(section_table, section_table_size, metadata.headers_size)) {
        return {{}, "PE section table exceeds the image or declared headers"};
    }

    metadata.sections.reserve(section_count);
    bool entry_is_executable = false;
    for (std::uint16_t i = 0; i < section_count; ++i) {
        const auto offset = section_table + static_cast<std::size_t>(i) * kSectionHeaderSize;
        PeSectionMetadata section{};
        for (std::size_t n = 0; n < section.name.size(); ++n) {
            section.name[n] = static_cast<char>(std::to_integer<unsigned char>(image[offset + n]));
        }
        if (!read_le32(image, offset + 8U, section.virtual_size) ||
            !read_le32(image, offset + 12U, section.virtual_address) ||
            !read_le32(image, offset + 16U, section.raw_size) ||
            !read_le32(image, offset + 20U, section.raw_offset) ||
            !read_le32(image, offset + 36U, section.characteristics)) {
            return {{}, "PE section table entry is truncated"};
        }
        const auto virtual_extent = std::max(section.virtual_size, section.raw_size);
        if (!range_fits(section.virtual_address, virtual_extent, metadata.image_size) ||
            !range_fits(section.raw_offset, section.raw_size, image.size())) {
            return {{}, "PE section points outside the image"};
        }
        if ((section.characteristics & kSectionExecutable) != 0U &&
            metadata.entry_point_rva >= section.virtual_address &&
            metadata.entry_point_rva - section.virtual_address < virtual_extent) {
            entry_is_executable = true;
        }
        metadata.sections.push_back(section);
    }
    if (!entry_is_executable) return {{}, "PE entry point is not inside an executable section"};
    return {metadata, {}};
}

} // namespace project360::loader
