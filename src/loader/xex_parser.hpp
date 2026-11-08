#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace project360::loader {

struct XexMetadata {
    std::uint32_t module_flags{};
    std::uint32_t header_size{};
    std::uint32_t security_info_offset{};
    std::uint32_t image_size{};
    std::uint32_t optional_header_count{};
    std::optional<std::uint32_t> title_id;
    std::optional<std::uint32_t> media_id;
    std::optional<std::uint32_t> entry_point;
};

struct XexParseResult {
    std::optional<XexMetadata> metadata;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return metadata.has_value(); }
};

// Parses unencrypted XEX2 container metadata only; it does not decrypt or load an image.
[[nodiscard]] XexParseResult parse_xex2(std::span<const std::byte> file);

} // namespace project360::loader
