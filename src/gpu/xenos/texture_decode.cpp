#include "gpu/xenos/texture_decode.hpp"

#include "gpu/xenos/texture_fetch.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace project360::gpu::xenos {
namespace {

std::uint16_t read_u16_le(std::span<const std::byte> bytes, std::size_t offset) {
    const auto low = std::to_integer<std::uint8_t>(bytes[offset]);
    const auto high = std::to_integer<std::uint8_t>(bytes[offset + 1U]);
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(low) |
                                      (static_cast<std::uint16_t>(high) << 8U));
}

std::uint32_t read_u32_le(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U]))
            << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U]))
            << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U]))
            << 24U);
}

std::uint64_t read_u64_le(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i]))
                 << static_cast<std::uint32_t>(i * 8U);
    }
    return value;
}

std::uint8_t expand_5_to_8(std::uint32_t value) {
    return static_cast<std::uint8_t>((value << 3U) | (value >> 2U));
}

std::uint8_t expand_6_to_8(std::uint32_t value) {
    return static_cast<std::uint8_t>((value << 2U) | (value >> 4U));
}

TextureRgba8 decode_565(std::uint16_t value) {
    return {expand_5_to_8((value >> 11U) & 0x1fU),
            expand_6_to_8((value >> 5U) & 0x3fU),
            expand_5_to_8(value & 0x1fU), 255U};
}

std::uint8_t interpolate(std::uint8_t first, std::uint8_t second,
                         std::uint32_t first_weight, std::uint32_t second_weight,
                         std::uint32_t divisor) {
    return static_cast<std::uint8_t>(
        (first_weight * first + second_weight * second) / divisor);
}

std::array<TextureRgba8, 16> decode_color_block(std::span<const std::byte> bytes,
                                               bool allow_transparent_selector) {
    const auto color0_word = read_u16_le(bytes, 0U);
    const auto color1_word = read_u16_le(bytes, 2U);
    const auto color0 = decode_565(color0_word);
    const auto color1 = decode_565(color1_word);

    std::array<TextureRgba8, 4> palette{color0, color1, {}, {}};
    if (!allow_transparent_selector || color0_word > color1_word) {
        palette[2] = {interpolate(color0.r, color1.r, 2U, 1U, 3U),
                      interpolate(color0.g, color1.g, 2U, 1U, 3U),
                      interpolate(color0.b, color1.b, 2U, 1U, 3U), 255U};
        palette[3] = {interpolate(color0.r, color1.r, 1U, 2U, 3U),
                      interpolate(color0.g, color1.g, 1U, 2U, 3U),
                      interpolate(color0.b, color1.b, 1U, 2U, 3U), 255U};
    } else {
        palette[2] = {interpolate(color0.r, color1.r, 1U, 1U, 2U),
                      interpolate(color0.g, color1.g, 1U, 1U, 2U),
                      interpolate(color0.b, color1.b, 1U, 1U, 2U), 255U};
        palette[3] = {0U, 0U, 0U, 0U};
    }

    const auto indices = read_u32_le(bytes, 4U);
    std::array<TextureRgba8, 16> texels{};
    for (std::size_t texel = 0U; texel < texels.size(); ++texel) {
        const auto shift = static_cast<std::uint32_t>(texel * 2U);
        const auto index = (indices >> shift) & 0x3U;
        texels[texel] = palette[index];
    }
    return texels;
}

TextureBlockDecodeResult decode_dxt1(std::span<const std::byte> bytes) {
    TextureBlockDecodeResult result{};
    result.texels = decode_color_block(bytes, true);
    result.texel_count = 16U;
    return result;
}

TextureBlockDecodeResult decode_dxt3(std::span<const std::byte> bytes) {
    const auto alpha_bits = read_u64_le(bytes, 0U);
    TextureBlockDecodeResult result{};
    result.texels = decode_color_block(bytes.subspan(8U, 8U), false);
    result.texel_count = 16U;
    for (std::size_t texel = 0U; texel < result.texel_count; ++texel) {
        const auto shift = static_cast<std::uint32_t>(texel * 4U);
        const auto alpha4 = static_cast<std::uint8_t>((alpha_bits >> shift) & 0xfU);
        result.texels[texel].a = static_cast<std::uint8_t>(alpha4 * 17U);
    }
    return result;
}

TextureBlockDecodeResult decode_dxt5(std::span<const std::byte> bytes) {
    const auto alpha0 = std::to_integer<std::uint8_t>(bytes[0]);
    const auto alpha1 = std::to_integer<std::uint8_t>(bytes[1]);
    std::array<std::uint8_t, 8> alpha_palette{alpha0, alpha1};
    if (alpha0 > alpha1) {
        for (std::uint32_t i = 1U; i <= 6U; ++i) {
            alpha_palette[i + 1U] = interpolate(
                alpha0, alpha1, 7U - i, i, 7U);
        }
    } else {
        for (std::uint32_t i = 1U; i <= 4U; ++i) {
            alpha_palette[i + 1U] = interpolate(
                alpha0, alpha1, 5U - i, i, 5U);
        }
        alpha_palette[6] = 0U;
        alpha_palette[7] = 255U;
    }

    const auto alpha_indices = read_u64_le(bytes, 0U) >> 16U;
    TextureBlockDecodeResult result{};
    result.texels = decode_color_block(bytes.subspan(8U, 8U), false);
    result.texel_count = 16U;
    for (std::size_t texel = 0U; texel < result.texel_count; ++texel) {
        const auto shift = static_cast<std::uint32_t>(texel * 3U);
        const auto index = static_cast<std::size_t>((alpha_indices >> shift) & 0x7U);
        result.texels[texel].a = alpha_palette[index];
    }
    return result;
}

} // namespace

TextureBlockDecodeResult decode_xenos_texture_block_rgba8(
    std::uint8_t format, std::span<const std::byte> block_bytes) {
    const auto storage = decode_texture_storage_layout(format);
    if (!storage) return {{}, 0U, storage.error};
    if (block_bytes.size() != storage.layout.bytes_per_block) {
        return {{}, 0U, "Texture block byte count does not match its decoded storage format"};
    }

    if (format == 6U) {
        const auto packed = read_u32_le(block_bytes, 0U);
        TextureBlockDecodeResult result{};
        result.texels[0] = {
            static_cast<std::uint8_t>((packed >> 16U) & 0xffU),
            static_cast<std::uint8_t>((packed >> 8U) & 0xffU),
            static_cast<std::uint8_t>(packed & 0xffU),
            static_cast<std::uint8_t>((packed >> 24U) & 0xffU)};
        result.texel_count = 1U;
        return result;
    }
    if (format == 4U) {
        TextureBlockDecodeResult result{};
        result.texels[0] = decode_565(read_u16_le(block_bytes, 0U));
        result.texel_count = 1U;
        return result;
    }
    if (format == 18U) return decode_dxt1(block_bytes);
    if (format == 19U) return decode_dxt3(block_bytes);
    if (format == 20U) return decode_dxt5(block_bytes);

    return {{}, 0U, "RGBA8 decoding is not implemented for texture format " +
                          std::to_string(format)};
}

} // namespace project360::gpu::xenos
