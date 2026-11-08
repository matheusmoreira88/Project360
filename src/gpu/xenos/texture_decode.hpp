#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace project360::gpu::xenos {

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

// Decodes one complete storage block into row-major RGBA8 texels for the
// explicitly supported Xenos formats. Numeric sign, gamma, component swizzle,
// filtering and sampling semantics are not applied.
[[nodiscard]] TextureBlockDecodeResult decode_xenos_texture_block_rgba8(
    std::uint8_t format, std::span<const std::byte> block_bytes);

} // namespace project360::gpu::xenos
