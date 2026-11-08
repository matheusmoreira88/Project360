#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace project360::memory {
class GuestMemory;
}

namespace project360::loader {

struct XexImportLibrary {
    std::string name;
    std::uint8_t library_index{};
    std::uint32_t version{};
    std::uint32_t minimum_version{};
    std::vector<std::uint32_t> import_record_addresses;
    std::vector<std::optional<std::uint8_t>> import_types;
    std::vector<std::optional<std::uint16_t>> import_ordinals;
};

struct XexMetadata {
    std::uint32_t module_flags{};
    std::uint32_t header_size{};
    std::uint32_t security_info_offset{};
    std::uint32_t image_size{};
    std::uint32_t security_load_address{};
    std::uint32_t optional_header_count{};
    std::optional<std::uint32_t> title_id;
    std::optional<std::uint32_t> media_id;
    std::optional<std::uint32_t> entry_point;
    std::optional<std::uint32_t> image_base_address;
    std::optional<std::uint16_t> encryption_type;
    std::optional<std::uint16_t> compression_type;
    std::vector<XexImportLibrary> import_libraries;
};

struct XexParseResult {
    std::optional<XexMetadata> metadata;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return metadata.has_value(); }
};

// Parses unencrypted XEX2 container metadata only; it does not decrypt or load an image.
[[nodiscard]] XexParseResult parse_xex2(std::span<const std::byte> file);
[[nodiscard]] XexParseResult parse_xex2_file(const std::filesystem::path& path);

struct PeSectionMetadata {
    std::array<char, 8> name{};
    std::uint32_t virtual_address{};
    std::uint32_t virtual_size{};
    std::uint32_t raw_offset{};
    std::uint32_t raw_size{};
    std::uint32_t characteristics{};
};

struct XboxPeMetadata {
    std::uint32_t entry_point_rva{};
    std::uint32_t image_base{};
    std::uint32_t section_alignment{};
    std::uint32_t file_alignment{};
    std::uint32_t image_size{};
    std::uint32_t headers_size{};
    std::vector<PeSectionMetadata> sections;
};

struct PeParseResult {
    std::optional<XboxPeMetadata> metadata;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return metadata.has_value(); }
};

// Parses an already-decoded Xbox 360 PowerPC big-endian PE32 image.
[[nodiscard]] PeParseResult parse_xbox_pe32(std::span<const std::byte> image);

struct LoadedXexImage {
    XexMetadata xex;
    XboxPeMetadata pe;
    std::uint32_t image_base{};
    std::uint32_t entry_point{};
    std::size_t mapped_size{};
};

struct XexLoadResult {
    std::optional<LoadedXexImage> image;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return image.has_value(); }
};

// Loads only unencrypted, uncompressed XEX2 images into a caller-provided guest memory map.
[[nodiscard]] XexLoadResult load_xex2_uncompressed(std::span<const std::byte> file,
                                                   memory::GuestMemory& memory,
                                                   std::uint64_t physical_base);
[[nodiscard]] XexLoadResult load_xex2_file(const std::filesystem::path& path,
                                           memory::GuestMemory& memory,
                                           std::uint64_t physical_base);

} // namespace project360::loader
