#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace project360::gpu::xenos {

struct TextureFetchConstant;

struct TextureRgba8 {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a{255U};

    friend bool operator==(const TextureRgba8&, const TextureRgba8&) = default;
};

struct TextureBlockDecodeResult {
    std::array<TextureRgba8, 16> texels{};
    std::uint8_t texel_count{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct TextureFloat4 {
    float r{};
    float g{};
    float b{};
    float a{};

    friend bool operator==(const TextureFloat4&, const TextureFloat4&) = default;
};

struct TextureFloatBlockDecodeResult {
    std::array<TextureFloat4, 16> texels{};
    std::uint8_t texel_count{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Decodes one complete storage block into row-major RGBA8 texels for the
// explicitly supported Xenos formats (8:8:8:8, 5:6:5, DXT1, DXT3 and DXT5).
// Numeric sign, gamma, filtering and sampling semantics are not applied.
[[nodiscard]] TextureBlockDecodeResult decode_xenos_texture_block_rgba8(
    std::uint8_t format, std::span<const std::byte> block_bytes);

// Applies the fetch descriptor's destination component selection to every
// decoded texel. Sign modes remain associated with source components and must
// be resolved by the sampling stage before this selection is applied.
[[nodiscard]] TextureBlockDecodeResult apply_texture_fetch_component_swizzle(
    const TextureFetchConstant& fetch, const TextureBlockDecodeResult& decoded);

// Interprets raw format-6 (8:8:8:8) channels with the descriptor's numeric
// sign/number modes, then applies its destination swizzle. Other storage
// formats fail until their component widths and signed encodings are handled.
[[nodiscard]] TextureFloatBlockDecodeResult decode_xenos_texture_fetch_block_float(
    std::uint8_t format, const TextureFetchConstant& fetch,
    const TextureBlockDecodeResult& decoded);

} // namespace project360::gpu::xenos
