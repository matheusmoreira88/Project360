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

TextureBlockDecodeResult decode_dxt1(std::span<const std::byte> bytes) {
    const auto color0_word = read_u16_le(bytes, 0U);
    const auto color1_word = read_u16_le(bytes, 2U);
    const auto color0 = decode_565(color0_word);
    const auto color1 = decode_565(color1_word);

    std::array<TextureRgba8, 4> palette{color0, color1, {}, {}};
    const auto interpolate = [](std::uint8_t first, std::uint8_t second,
                                std::uint32_t first_weight,
                                std::uint32_t second_weight,
                                std::uint32_t divisor) {
        return static_cast<std::uint8_t>(
            (first_weight * first + second_weight * second) / divisor);
    };
    if (color0_word > color1_word) {
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
    TextureBlockDecodeResult result{};
    result.texel_count = 16U;
    for (std::size_t texel = 0U; texel < result.texel_count; ++texel) {
        const auto shift = static_cast<std::uint32_t>(texel * 2U);
        const auto index = (indices >> shift) & 0x3U;
        result.texels[texel] = palette[index];
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
    if (format == 18U) return decode_dxt1(block_bytes);

    return {{}, 0U, "RGBA8 decoding is not implemented for texture format " +
                          std::to_string(format)};
}

} // namespace project360::gpu::xenos
