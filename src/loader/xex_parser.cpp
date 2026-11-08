#include "loader/xex_parser.hpp"

#include "memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace project360::loader {
namespace {

constexpr std::uint32_t kXex1Magic = 0x58455831U;
constexpr std::uint32_t kXex2Magic = 0x58455832U;
constexpr std::uint32_t kXexpMagic = 0x58455825U;
constexpr std::uint32_t kExecutionInfoKey = 0x00040006U;
constexpr std::uint32_t kEntryPointKey = 0x00010100U;
constexpr std::uint32_t kImageBaseAddressKey = 0x00010201U;
constexpr std::uint32_t kFileFormatInfoKey = 0x000003ffU;
constexpr std::uint32_t kImportLibrariesKey = 0x000103ffU;
constexpr std::size_t kFixedHeaderSize = 0x18U;
constexpr std::size_t kOptionalHeaderEntrySize = 8U;
constexpr std::size_t kExecutionInfoSize = 0x18U;
constexpr std::size_t kSecurityInfoMinimumSize = 0x184U;
constexpr std::uint32_t kMaximumHeaderSize = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumImportLibraryNames = 256U;
constexpr std::size_t kMaximumImportLibraries = 256U;

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

bool read_be16(std::span<const std::byte> bytes, std::size_t offset, std::uint16_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) return false;
    value = static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(bytes[offset]) << 8U) |
                                       std::to_integer<std::uint16_t>(bytes[offset + 1U]));
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

bool decode_import_records(std::span<const std::byte> pe_bytes, const XboxPeMetadata& pe,
                           std::uint64_t image_base, bool image_is_memory_layout,
                           XexMetadata& xex,
                           std::string& error) {
    for (auto& library : xex.import_libraries) {
        library.import_types.reserve(library.import_record_addresses.size());
        library.import_ordinals.reserve(library.import_record_addresses.size());
        for (const auto record_address : library.import_record_addresses) {
            const auto address = static_cast<std::uint64_t>(record_address);
            if ((record_address & 3U) != 0U || address < image_base ||
                !range_fits(address - image_base, 4U, pe.image_size)) {
                error = "XEX import record address is unaligned or outside the PE image";
                return false;
            }
            const auto rva = address - image_base;
            std::uint64_t raw_offset = 0;
            bool found_raw_data = false;
            if (image_is_memory_layout && range_fits(rva, 4U, pe_bytes.size())) {
                raw_offset = rva;
                found_raw_data = true;
            } else if (range_fits(rva, 4U, pe.headers_size)) {
                raw_offset = rva;
                found_raw_data = true;
            } else {
                for (const auto& section : pe.sections) {
                    if (rva < section.virtual_address) continue;
                    const auto section_offset = rva - section.virtual_address;
                    if (range_fits(section_offset, 4U, section.raw_size)) {
                        raw_offset = static_cast<std::uint64_t>(section.raw_offset) +
                                     section_offset;
                        found_raw_data = true;
                        break;
                    }
                }
            }
            std::uint32_t import_record = 0;
            if (!found_raw_data || !read_be32(pe_bytes, static_cast<std::size_t>(raw_offset),
                                              import_record)) {
                error = "XEX import record does not point to four backed PE bytes";
                return false;
            }
            const auto type = static_cast<std::uint8_t>(import_record >> 24U);
            const auto library_index = static_cast<std::uint8_t>((import_record >> 16U) & 0xffU);
            const auto ordinal = static_cast<std::uint16_t>(import_record & 0xffffU);
            if (library_index != library.library_index) {
                error = "XEX import record library index disagrees with its descriptor";
                return false;
            }
            if (type > 1U) {
                error = "XEX import record type is unsupported";
                return false;
            }
            library.import_types.emplace_back(type);
            library.import_ordinals.emplace_back(ordinal);
        }
    }
    return true;
}

bool parse_import_libraries(std::span<const std::byte> file, std::uint32_t offset,
                            std::uint32_t header_size,
                            std::vector<XexImportLibrary>& libraries, std::string& error) {
    std::uint32_t block_size = 0;
    std::uint32_t string_table_size = 0;
    std::uint32_t string_count = 0;
    if (!range_fits(offset, 12U, header_size) || !read_be32(file, offset, block_size) ||
        !read_be32(file, static_cast<std::size_t>(offset) + 4U, string_table_size) ||
        !read_be32(file, static_cast<std::size_t>(offset) + 8U, string_count) ||
        block_size < 12U || !range_fits(offset, block_size, header_size) ||
        string_table_size > block_size - 12U || string_count > string_table_size ||
        string_count > kMaximumImportLibraryNames) {
        error = "XEX import-library header or string table has invalid bounds";
        return false;
    }

    const auto table_begin = static_cast<std::size_t>(offset) + 12U;
    const auto table_size = static_cast<std::size_t>(string_table_size);
    std::vector<std::string> names;
    names.reserve(string_count);
    std::size_t cursor = 0;
    while (names.size() < string_count) {
        if (cursor >= table_size) {
            error = "XEX import-library string table has fewer names than declared";
            return false;
        }
        const auto begin = cursor;
        while (cursor < table_size && file[table_begin + cursor] != std::byte{0} &&
               file[table_begin + cursor] != std::byte{0xcd}) {
            ++cursor;
        }
        if (cursor == table_size || cursor == begin) {
            error = "XEX import-library name is empty or not terminated";
            return false;
        }
        std::string name;
        name.reserve(cursor - begin);
        for (auto index = begin; index < cursor; ++index) {
            const auto value = std::to_integer<unsigned char>(file[table_begin + index]);
            if (value < 0x20U || value > 0x7eU) {
                error = "XEX import-library name contains non-ASCII bytes";
                return false;
            }
            name.push_back(static_cast<char>(value));
        }
        names.push_back(std::move(name));
        ++cursor;
        const auto aligned = (cursor + 3U) & ~std::size_t{3U};
        if (aligned > table_size) {
            error = "XEX import-library string padding exceeds its table";
            return false;
        }
        while (cursor < aligned) {
            const auto padding = file[table_begin + cursor];
            if (padding != std::byte{0} && padding != std::byte{0xcd}) {
                error = "XEX import-library string padding contains unexpected data";
                return false;
            }
            ++cursor;
        }
    }
    while (cursor < table_size) {
        const auto padding = file[table_begin + cursor];
        if (padding != std::byte{0} && padding != std::byte{0xcd}) {
            error = "XEX import-library string table has trailing data";
            return false;
        }
        ++cursor;
    }

    const auto block_end = static_cast<std::size_t>(offset) + block_size;
    cursor = table_begin + table_size;
    while (cursor < block_end) {
        if (block_end - cursor < 4U) {
            error = "XEX import-library descriptor is truncated";
            return false;
        }
        std::uint32_t library_size = 0;
        if (!read_be32(file, cursor, library_size)) {
            error = "XEX import-library descriptor is truncated";
            return false;
        }
        if (library_size == 0U) {
            while (cursor < block_end) {
                if (file[cursor] != std::byte{0} && file[cursor] != std::byte{0xcd}) {
                    error = "XEX import-library terminator has trailing data";
                    return false;
                }
                ++cursor;
            }
            break;
        }
        constexpr std::size_t kLibraryFixedSize = 0x28U;
        if (library_size < kLibraryFixedSize || (library_size & 3U) != 0U ||
            library_size > block_end - cursor || libraries.size() >= kMaximumImportLibraries) {
            error = "XEX import-library descriptor size is invalid";
            return false;
        }
        std::uint32_t version = 0;
        std::uint32_t minimum_version = 0;
        std::uint16_t name_index = 0;
        std::uint16_t import_count = 0;
        if (!read_be32(file, cursor + 0x1cU, version) ||
            !read_be32(file, cursor + 0x20U, minimum_version) ||
            !read_be16(file, cursor + 0x24U, name_index) ||
            !read_be16(file, cursor + 0x26U, import_count)) {
            error = "XEX import-library descriptor fields are truncated";
            return false;
        }
        const auto indexed_name = static_cast<std::size_t>(name_index & 0xffU);
        const auto import_bytes = static_cast<std::uint64_t>(import_count) * 4U;
        if (indexed_name >= names.size() ||
            !range_fits(kLibraryFixedSize, import_bytes, library_size)) {
            error = "XEX import-library name index or import count is invalid";
            return false;
        }
        XexImportLibrary library{};
        library.name = names[indexed_name];
        library.library_index = static_cast<std::uint8_t>(indexed_name);
        library.version = version;
        library.minimum_version = minimum_version;
        library.import_record_addresses.reserve(import_count);
        for (std::uint16_t i = 0; i < import_count; ++i) {
            std::uint32_t record_address = 0;
            if (!read_be32(file, cursor + kLibraryFixedSize +
                                     static_cast<std::size_t>(i) * 4U,
                           record_address)) {
                error = "XEX import record address is truncated";
                return false;
            }
            library.import_record_addresses.push_back(record_address);
        }
        libraries.push_back(std::move(library));
        cursor += library_size;
    }
    return true;
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
    std::uint32_t security_load_address = 0;
    if (!read_be32(file, security_offset, security_size) ||
        !read_be32(file, static_cast<std::size_t>(security_offset) + 4U, image_size) ||
        !read_be32(file, static_cast<std::size_t>(security_offset) + 0x110U,
                   security_load_address) ||
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
    metadata.security_load_address = security_load_address;
    metadata.optional_header_count = header_count;
    bool found_execution_info = false;
    bool found_file_format_info = false;
    bool found_import_libraries = false;
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
        } else if (key == kImageBaseAddressKey) {
            if (metadata.image_base_address) return {{}, "duplicate XEX image-base header"};
            metadata.image_base_address = value;
        } else if (key == kFileFormatInfoKey) {
            if (found_file_format_info) return {{}, "duplicate XEX file-format header"};
            found_file_format_info = true;
            std::uint32_t info_size = 0;
            std::uint16_t encryption_type = 0;
            std::uint16_t compression_type = 0;
            if (!range_fits(value, 8U, header_size) || !read_be32(file, value, info_size) ||
                info_size < 8U || !range_fits(value, info_size, header_size) ||
                !read_be16(file, static_cast<std::size_t>(value) + 4U, encryption_type) ||
                !read_be16(file, static_cast<std::size_t>(value) + 6U, compression_type)) {
                return {{}, "XEX file-format info is truncated or out of bounds"};
            }
            metadata.encryption_type = encryption_type;
            metadata.compression_type = compression_type;
            if (compression_type == 1U) {
                if (info_size == 8U || ((info_size - 8U) % 8U) != 0U) {
                    return {{}, "XEX basic-compression block table has an invalid size"};
                }
                const auto block_count = (info_size - 8U) / 8U;
                metadata.basic_compression_blocks.reserve(block_count);
                for (std::uint32_t block = 0; block < block_count; ++block) {
                    std::uint32_t data_size = 0;
                    std::uint32_t zero_size = 0;
                    const auto block_offset = static_cast<std::size_t>(value) + 8U +
                                              static_cast<std::size_t>(block) * 8U;
                    if (!read_be32(file, block_offset, data_size) ||
                        !read_be32(file, block_offset + 4U, zero_size)) {
                        return {{}, "XEX basic-compression block table is truncated"};
                    }
                    metadata.basic_compression_blocks.push_back({data_size, zero_size});
                }
            }
        } else if (key == kImportLibrariesKey) {
            if (found_import_libraries) {
                return {{}, "duplicate XEX import-libraries header"};
            }
            found_import_libraries = true;
            std::string error;
            if (!parse_import_libraries(file, value, header_size,
                                        metadata.import_libraries, error)) {
                return {{}, std::move(error)};
            }
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
        if (section.raw_size != 0U && section.raw_offset < metadata.headers_size) {
            return {{}, "PE section raw data overlaps the declared headers"};
        }
        for (const auto& previous : metadata.sections) {
            const auto previous_virtual_extent =
                std::max(previous.virtual_size, previous.raw_size);
            if (virtual_extent != 0U && previous_virtual_extent != 0U &&
                section.virtual_address <
                    static_cast<std::uint64_t>(previous.virtual_address) +
                        previous_virtual_extent &&
                previous.virtual_address <
                    static_cast<std::uint64_t>(section.virtual_address) + virtual_extent) {
                return {{}, "PE sections have overlapping virtual ranges"};
            }
            if (section.raw_size != 0U && previous.raw_size != 0U &&
                section.raw_offset <
                    static_cast<std::uint64_t>(previous.raw_offset) + previous.raw_size &&
                previous.raw_offset <
                    static_cast<std::uint64_t>(section.raw_offset) + section.raw_size) {
                return {{}, "PE sections have overlapping raw-data ranges"};
            }
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

XexLoadResult load_xex2(std::span<const std::byte> file, memory::GuestMemory& memory,
                        std::uint64_t physical_base) {
    constexpr std::uint64_t kMaximumImageSize = 512ULL * 1024ULL * 1024ULL;
    constexpr std::size_t kPageSize = static_cast<std::size_t>(memory::GuestMemory::page_size);
    constexpr std::uint32_t kSectionExecutable = 0x20000000U;
    constexpr std::uint32_t kSectionReadable = 0x40000000U;
    constexpr std::uint32_t kSectionWritable = 0x80000000U;

    const auto xex_result = parse_xex2(file);
    if (!xex_result) return {{}, xex_result.error};
    const auto& xex = *xex_result.metadata;
    if (!xex.encryption_type || !xex.compression_type) {
        return {{}, "XEX2 file-format information is required to load the image"};
    }
    if (*xex.encryption_type != 0U) {
        return {{}, "encrypted XEX2 images are unsupported; no decryption was attempted"};
    }
    if (xex.header_size >= file.size()) return {{}, "XEX2 image payload is empty"};
    std::vector<std::byte> decoded_image;
    std::span<const std::byte> pe_bytes;
    const auto payload = file.subspan(xex.header_size);
    const bool basic_compression = *xex.compression_type == 1U;
    if (*xex.compression_type == 0U) {
        pe_bytes = payload;
    } else if (basic_compression) {
        if (xex.image_size == 0U || xex.image_size > kMaximumImageSize ||
            xex.basic_compression_blocks.empty()) {
            return {{}, "XEX basic-compression image size or block table is invalid"};
        }
        std::uint64_t total_output_size = 0;
        std::uint64_t total_input_size = 0;
        for (const auto& block : xex.basic_compression_blocks) {
            total_output_size += static_cast<std::uint64_t>(block.data_size) + block.zero_size;
            total_input_size += block.data_size;
            if (total_output_size > xex.image_size || total_input_size > payload.size()) {
                return {{}, "XEX basic-compression blocks exceed declared image or payload bounds"};
            }
        }
        if (total_output_size == 0U) {
            return {{}, "XEX basic-compression blocks produce an empty image"};
        }
        try {
            decoded_image.reserve(static_cast<std::size_t>(total_output_size));
            std::size_t input_offset = 0;
            for (const auto& block : xex.basic_compression_blocks) {
                const auto data_size = static_cast<std::size_t>(block.data_size);
                decoded_image.insert(decoded_image.end(), payload.begin() + input_offset,
                                     payload.begin() + input_offset + data_size);
                decoded_image.insert(decoded_image.end(), block.zero_size, std::byte{0});
                input_offset += data_size;
            }
        } catch (const std::bad_alloc&) {
            return {{}, "not enough host memory to decode XEX basic-compression blocks"};
        }
        pe_bytes = decoded_image;
    } else {
        return {{}, "XEX compression type is unsupported; only none and basic are implemented"};
    }
    const auto pe_result = parse_xbox_pe32(pe_bytes);
    if (!pe_result) return {{}, pe_result.error};
    const auto& pe = *pe_result.metadata;
    const std::uint64_t image_base =
        xex.image_base_address.value_or(xex.security_load_address);
    if (image_base != pe.image_base) {
        return {{}, "XEX image base differs from PE preferred base; relocations are unsupported"};
    }
    XexMetadata loaded_xex{};
    try {
        loaded_xex = xex;
    } catch (const std::bad_alloc&) {
        return {{}, "not enough host memory to retain XEX import metadata"};
    }
    std::string import_error;
    try {
        if (!decode_import_records(pe_bytes, pe, image_base, basic_compression, loaded_xex,
                                   import_error)) {
            return {{}, std::move(import_error)};
        }
    } catch (const std::bad_alloc&) {
        return {{}, "not enough host memory to decode XEX import records"};
    }
    if ((image_base & (memory::GuestMemory::page_size - 1U)) != 0U || pe.image_size == 0U ||
        pe.image_size > kMaximumImageSize ||
        image_base + pe.image_size > 0x100000000ULL) {
        return {{}, "Xbox PE image base or mapped size is outside the supported range"};
    }

    const auto mapped_size =
        (static_cast<std::uint64_t>(pe.image_size) + kPageSize - 1U) &
        ~(static_cast<std::uint64_t>(kPageSize) - 1U);
    if (image_base + mapped_size > 0x100000000ULL) {
        return {{}, "rounded Xbox PE page range exceeds the 32-bit guest address space"};
    }
    if (physical_base % kPageSize != 0U || mapped_size > memory.size() ||
        !memory.contains(physical_base, static_cast<std::size_t>(mapped_size))) {
        return {{}, "guest physical backing range is too small or misaligned"};
    }
    if (memory.physical_range_mapped(physical_base, static_cast<std::size_t>(mapped_size))) {
        return {{}, "guest physical backing range overlaps an existing mapping"};
    }
    const auto page_count = static_cast<std::size_t>(mapped_size / kPageSize);
    std::vector<memory::GuestPageAccess> page_access;
    try {
        page_access.assign(page_count, memory::GuestPageAccess::none);
    } catch (const std::bad_alloc&) {
        return {{}, "not enough host memory for XEX page permissions"};
    }
    const auto header_page_count =
        (static_cast<std::size_t>(pe.headers_size) + kPageSize - 1U) / kPageSize;
    for (std::size_t page = 0; page < header_page_count; ++page) {
        page_access[page] = memory::GuestPageAccess::read;
    }
    for (const auto& section : pe.sections) {
        const auto extent = std::max(section.virtual_size, section.raw_size);
        if (extent == 0U) continue;
        auto access = memory::GuestPageAccess::none;
        if ((section.characteristics & kSectionReadable) != 0U) {
            access = access | memory::GuestPageAccess::read;
        }
        if ((section.characteristics & kSectionWritable) != 0U) {
            access = access | memory::GuestPageAccess::write;
        }
        if ((section.characteristics & kSectionExecutable) != 0U) {
            access = access | memory::GuestPageAccess::execute;
        }
        const auto first_page = static_cast<std::size_t>(section.virtual_address / kPageSize);
        const auto last_byte = static_cast<std::uint64_t>(section.virtual_address) + extent - 1U;
        const auto last_page = static_cast<std::size_t>(last_byte / kPageSize);
        if (last_page >= page_access.size()) return {{}, "PE section exceeds mapped image pages"};
        for (auto page = first_page; page <= last_page; ++page) {
            page_access[page] = page_access[page] | access;
        }
    }

    const auto entry_point = xex.entry_point.value_or(
        static_cast<std::uint32_t>(image_base + pe.entry_point_rva));
    if (entry_point < image_base || entry_point >= image_base + pe.image_size) {
        return {{}, "XEX entry point is outside the PE image"};
    }
    const auto entry_rva = static_cast<std::uint32_t>(entry_point - image_base);
    const auto entry_section = std::find_if(pe.sections.begin(), pe.sections.end(),
                                            [&](const PeSectionMetadata& section) {
        const auto extent = std::max(section.virtual_size, section.raw_size);
        return (section.characteristics & kSectionExecutable) != 0U &&
               entry_rva >= section.virtual_address &&
               static_cast<std::uint64_t>(entry_rva) - section.virtual_address < extent;
    });
    if (entry_section == pe.sections.end()) {
        return {{}, "XEX entry point is not inside an executable PE section"};
    }

    std::vector<memory::GuestPageMapping> mappings;
    try {
        mappings.reserve(page_count);
        for (std::size_t page = 0; page < page_count; ++page) {
            mappings.push_back({image_base + page * kPageSize,
                                physical_base + page * kPageSize, page_access[page]});
        }
    } catch (const std::bad_alloc&) {
        return {{}, "not enough host memory to prepare XEX page mappings"};
    }
    if (!memory.map_pages(mappings)) {
        return {{}, "XEX image mappings overlap or could not be allocated"};
    }

    const std::array<std::byte, kPageSize> zero_page{};
    for (std::size_t offset = 0; offset < mapped_size; offset += kPageSize) {
        if (!memory.load(physical_base + offset, zero_page)) {
            for (std::size_t page = 0; page < page_count; ++page) {
                (void)memory.unmap_page(image_base + page * kPageSize);
            }
            return {{}, "failed to initialize mapped XEX image memory"};
        }
    }
    if (!memory.load(physical_base, pe_bytes.first(pe.headers_size))) {
        for (std::size_t page = 0; page < page_count; ++page) {
            (void)memory.unmap_page(image_base + page * kPageSize);
        }
        return {{}, "failed to copy PE headers into guest memory"};
    }
    for (const auto& section : pe.sections) {
        if (section.raw_size == 0U) continue;
        const auto source_offset = basic_compression ? section.virtual_address : section.raw_offset;
        if (!range_fits(source_offset, section.raw_size, pe_bytes.size())) {
            for (std::size_t page = 0; page < page_count; ++page) {
                (void)memory.unmap_page(image_base + page * kPageSize);
            }
            return {{}, "PE section data exceeds the decoded XEX image"};
        }
        const auto source = pe_bytes.subspan(source_offset, section.raw_size);
        if (!memory.load(physical_base + section.virtual_address, source)) {
            for (std::size_t page = 0; page < page_count; ++page) {
                (void)memory.unmap_page(image_base + page * kPageSize);
            }
            return {{}, "failed to copy a PE section into guest memory"};
        }
    }

    LoadedXexImage loaded{};
    loaded.xex = std::move(loaded_xex);
    loaded.pe = pe;
    loaded.image_base = static_cast<std::uint32_t>(image_base);
    loaded.entry_point = entry_point;
    loaded.mapped_size = static_cast<std::size_t>(mapped_size);
    return {std::move(loaded), {}};
}

XexLoadResult load_xex2_uncompressed(std::span<const std::byte> file,
                                     memory::GuestMemory& memory,
                                     std::uint64_t physical_base) {
    return load_xex2(file, memory, physical_base);
}

XexLoadResult load_xex2_file(const std::filesystem::path& path, memory::GuestMemory& memory,
                             std::uint64_t physical_base) {
    constexpr std::uint64_t kMaximumXexFileSize = 512ULL * 1024ULL * 1024ULL;
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) return {{}, "unable to read XEX file size: " + size_error.message()};
    if (file_size == 0U || file_size > kMaximumXexFileSize ||
        file_size > std::numeric_limits<std::size_t>::max()) {
        return {{}, "XEX file size is empty or exceeds the loader's 512 MiB limit"};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) return {{}, "unable to open XEX file for loading"};
    std::vector<std::byte> contents;
    try {
        contents.resize(static_cast<std::size_t>(file_size));
    } catch (const std::bad_alloc&) {
        return {{}, "not enough host memory to read the XEX file"};
    }
    input.read(reinterpret_cast<char*>(contents.data()),
               static_cast<std::streamsize>(contents.size()));
    if (input.gcount() != static_cast<std::streamsize>(contents.size())) {
        return {{}, "unable to read the complete XEX file"};
    }
    return load_xex2_uncompressed(contents, memory, physical_base);
}

} // namespace project360::loader
