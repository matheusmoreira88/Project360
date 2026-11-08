#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace project360::gpu::xenos {

enum class TextureDataDimension : std::uint8_t {
    one_d = 0U,
    two_d_or_stacked = 1U,
    three_d = 2U,
    cube = 3U
};

// Decoded metadata from a six-dword Xenos texture fetch constant. The raw
// words are retained because texture layout, format interpretation, and
// sampling behavior are not interpreted by this type yet.
struct TextureFetchConstant {
    std::array<std::uint32_t, 6> raw_words{};
    std::array<std::uint8_t, 4> sign_modes{};
    std::array<std::uint8_t, 3> clamp_modes{};
    std::uint16_t pitch{};
    std::uint8_t format{};
    std::uint8_t endian{};
    std::uint8_t request_size{};
    TextureDataDimension dimension{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth_or_layers{};
    std::uint32_t base_address_bytes{};
    std::uint32_t mip_address_bytes{};
    std::uint16_t component_swizzle{};
    std::int8_t exponent_adjust{};
    std::uint8_t number_format{};
    std::uint8_t magnification_filter{};
    std::uint8_t minification_filter{};
    std::uint8_t mip_filter{};
    std::uint8_t anisotropic_filter{};
    std::uint8_t arbitrary_filter{};
    std::uint8_t mip_min_level{};
    std::uint8_t mip_max_level{};
    std::int16_t lod_bias{};
    std::int8_t gradient_exponent_adjust_h{};
    std::int8_t gradient_exponent_adjust_v{};
    std::int8_t anisotropic_bias{};
    std::uint8_t border_color{};
    std::uint8_t tri_clamp{};
    bool tiled{};
    bool stacked{};
    bool nearest_clamp_policy{};
    bool border_size{};
    bool volume_magnification_filter{};
    bool volume_minification_filter{};
    bool magnification_anisotropic_walk{};
    bool minification_anisotropic_walk{};
    bool force_bc_w_to_max{};
    bool packed_mips{};
};

struct TextureFetchDecodeResult {
    TextureFetchConstant constant{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct TextureFetchConstantBankResult {
    std::array<TextureFetchConstant, 32> constants{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct TextureStorageLayout {
    std::uint8_t block_width_pixels{};
    std::uint8_t block_height_pixels{};
    std::uint8_t bytes_per_block{};
    bool compressed{};
};

struct TextureStorageLayoutResult {
    TextureStorageLayout layout{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Decodes the six-dword texture fetch descriptor. Width, height, and depth or
// layer counts are returned as texel counts after applying the stored minus-one
// encoding. Texture memory layout and sampling are not performed here.
[[nodiscard]] TextureFetchDecodeResult decode_texture_fetch_constant(
    const std::array<std::uint32_t, 6>& dwords);

// Resolves only requested texture slots from the shared FETCH_CONSTANT_0
// register bank. Texture slots contain six dwords; vertex slots use a separate
// two-dword interpretation of this same register space.
[[nodiscard]] TextureFetchConstantBankResult decode_texture_fetch_constant_bank(
    std::span<const std::uint32_t> register_file,
    std::span<const std::uint8_t> required_slots);

// Returns storage block geometry for the formats whose texel/block sizes are
// validated by the current implementation. Pixel/channel conversion is not
// included; unsupported format values fail explicitly.
[[nodiscard]] TextureStorageLayoutResult decode_texture_storage_layout(
    std::uint8_t format);

} // namespace project360::gpu::xenos
