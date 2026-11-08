#include "gpu/xenos/texture_fetch.hpp"

#include <cstddef>

namespace project360::gpu::xenos {
namespace {

constexpr std::uint32_t kFetchConstantTypeMask = 0x3U;
constexpr std::uint32_t kTextureFetchConstantType = 0x2U;
constexpr std::size_t kFetchConstantRegisterBase = 0x4800U;
constexpr std::size_t kTextureFetchDwordCount = 6U;
constexpr std::size_t kTextureFetchSlotCount = 32U;

std::int32_t sign_extend(std::uint32_t value, std::uint8_t width) {
    const auto sign_bit = 1U << (width - 1U);
    if ((value & sign_bit) == 0U) return static_cast<std::int32_t>(value);
    return static_cast<std::int32_t>(value) - static_cast<std::int32_t>(1U << width);
}

} // namespace

TextureFetchDecodeResult decode_texture_fetch_constant(
    const std::array<std::uint32_t, 6>& dwords) {
    if ((dwords[0] & kFetchConstantTypeMask) != kTextureFetchConstantType) {
        return {{}, "Fetch constant is not a Xenos texture descriptor"};
    }

    TextureFetchConstant constant{};
    constant.raw_words = dwords;

    for (std::size_t component = 0U; component < constant.sign_modes.size(); ++component) {
        const auto shift = static_cast<std::uint32_t>(2U + component * 2U);
        constant.sign_modes[component] =
            static_cast<std::uint8_t>((dwords[0] >> shift) & 0x3U);
    }
    for (std::size_t axis = 0U; axis < constant.clamp_modes.size(); ++axis) {
        const auto shift = static_cast<std::uint32_t>(10U + axis * 3U);
        constant.clamp_modes[axis] = static_cast<std::uint8_t>((dwords[0] >> shift) & 0x7U);
    }
    constant.pitch = static_cast<std::uint16_t>((dwords[0] >> 22U) & 0x1ffU);
    constant.tiled = (dwords[0] & 0x80000000U) != 0U;

    constant.format = static_cast<std::uint8_t>(dwords[1] & 0x3fU);
    constant.endian = static_cast<std::uint8_t>((dwords[1] >> 6U) & 0x3U);
    constant.request_size = static_cast<std::uint8_t>((dwords[1] >> 8U) & 0x3U);
    constant.stacked = (dwords[1] & (1U << 10U)) != 0U;
    constant.nearest_clamp_policy = (dwords[1] & (1U << 11U)) != 0U;
    constant.base_address_bytes = ((dwords[1] >> 12U) & 0x000fffffU) << 12U;

    constant.dimension = static_cast<TextureDataDimension>((dwords[5] >> 9U) & 0x3U);
    switch (constant.dimension) {
    case TextureDataDimension::one_d:
        constant.width = (dwords[2] & 0x00ffffffU) + 1U;
        constant.height = 1U;
        constant.depth_or_layers = 1U;
        break;
    case TextureDataDimension::two_d_or_stacked:
    case TextureDataDimension::cube:
        constant.width = (dwords[2] & 0x1fffU) + 1U;
        constant.height = ((dwords[2] >> 13U) & 0x1fffU) + 1U;
        constant.depth_or_layers = ((dwords[2] >> 26U) & 0x3fU) + 1U;
        break;
    case TextureDataDimension::three_d:
        constant.width = (dwords[2] & 0x7ffU) + 1U;
        constant.height = ((dwords[2] >> 11U) & 0x7ffU) + 1U;
        constant.depth_or_layers = ((dwords[2] >> 22U) & 0x3ffU) + 1U;
        break;
    }

    constant.number_format = static_cast<std::uint8_t>(dwords[3] & 0x1U);
    constant.component_swizzle = static_cast<std::uint16_t>((dwords[3] >> 1U) & 0xfffU);
    constant.exponent_adjust = static_cast<std::int8_t>(sign_extend((dwords[3] >> 13U) & 0x3fU, 6U));
    constant.magnification_filter = static_cast<std::uint8_t>((dwords[3] >> 19U) & 0x3U);
    constant.minification_filter = static_cast<std::uint8_t>((dwords[3] >> 21U) & 0x3U);
    constant.mip_filter = static_cast<std::uint8_t>((dwords[3] >> 23U) & 0x3U);
    constant.anisotropic_filter = static_cast<std::uint8_t>((dwords[3] >> 25U) & 0x7U);
    constant.arbitrary_filter = static_cast<std::uint8_t>((dwords[3] >> 28U) & 0x7U);
    constant.border_size = (dwords[3] & 0x80000000U) != 0U;

    constant.volume_magnification_filter = (dwords[4] & 0x1U) != 0U;
    constant.volume_minification_filter = (dwords[4] & 0x2U) != 0U;
    constant.mip_min_level = static_cast<std::uint8_t>((dwords[4] >> 2U) & 0xfU);
    constant.mip_max_level = static_cast<std::uint8_t>((dwords[4] >> 6U) & 0xfU);
    constant.magnification_anisotropic_walk = (dwords[4] & (1U << 10U)) != 0U;
    constant.minification_anisotropic_walk = (dwords[4] & (1U << 11U)) != 0U;
    constant.lod_bias = static_cast<std::int16_t>(sign_extend((dwords[4] >> 12U) & 0x3ffU, 10U));
    constant.gradient_exponent_adjust_h =
        static_cast<std::int8_t>(sign_extend((dwords[4] >> 22U) & 0x1fU, 5U));
    constant.gradient_exponent_adjust_v =
        static_cast<std::int8_t>(sign_extend((dwords[4] >> 27U) & 0x1fU, 5U));

    constant.border_color = static_cast<std::uint8_t>(dwords[5] & 0x3U);
    constant.force_bc_w_to_max = (dwords[5] & (1U << 2U)) != 0U;
    constant.tri_clamp = static_cast<std::uint8_t>((dwords[5] >> 3U) & 0x3U);
    constant.anisotropic_bias = static_cast<std::int8_t>(sign_extend((dwords[5] >> 5U) & 0xfU, 4U));
    constant.packed_mips = (dwords[5] & (1U << 11U)) != 0U;
    constant.mip_address_bytes = ((dwords[5] >> 12U) & 0x000fffffU) << 12U;

    return {constant, {}};
}

TextureFetchConstantBankResult decode_texture_fetch_constant_bank(
    std::span<const std::uint32_t> register_file,
    std::span<const std::uint8_t> required_slots) {
    TextureFetchConstantBankResult result{};
    for (const auto slot : required_slots) {
        if (slot >= kTextureFetchSlotCount) {
            return {{}, "Texture fetch slot exceeds the 32-entry Xenos bank"};
        }
        const auto first_register = kFetchConstantRegisterBase +
                                    static_cast<std::size_t>(slot) * kTextureFetchDwordCount;
        if (first_register > register_file.size() ||
            kTextureFetchDwordCount > register_file.size() - first_register) {
            return {{}, "Register file does not contain texture fetch slot " +
                            std::to_string(slot)};
        }
        std::array<std::uint32_t, 6> dwords{};
        for (std::size_t i = 0U; i < dwords.size(); ++i) {
            dwords[i] = register_file[first_register + i];
        }
        const auto decoded = decode_texture_fetch_constant(dwords);
        if (!decoded) {
            return {{}, "Texture fetch slot " + std::to_string(slot) + ": " + decoded.error};
        }
        result.constants[slot] = decoded.constant;
    }
    return result;
}

TextureStorageLayoutResult decode_texture_storage_layout(std::uint8_t format) {
    switch (format) {
    case 2U: // 8 bits per texel.
    case 8U:
    case 9U:
        return {{1U, 1U, 1U, false}, {}};
    case 3U: // 16 bits per texel.
    case 4U:
    case 5U:
    case 10U:
    case 15U:
        return {{1U, 1U, 2U, false}, {}};
    case 6U: // 32 bits per texel.
    case 7U:
    case 13U:
    case 14U:
    case 16U:
    case 17U:
    case 22U:
    case 23U:
    case 24U:
    case 25U:
    case 27U:
    case 28U:
    case 30U:
    case 31U:
    case 33U:
    case 36U:
    case 39U:
    case 40U:
    case 44U:
    case 45U:
    case 50U: // Same storage size as the corresponding 8:8:8:8 or 10:10:10:2 format.
    case 54U:
    case 55U:
    case 56U:
    case 62U:
    case 63U:
        return {{1U, 1U, 4U, false}, {}};
    case 19U: // 16-byte 4x4 DXT blocks.
    case 20U:
    case 52U:
    case 53U:
    case 49U: // Two 8-byte channel blocks per 4x4 DXN block.
        return {{4U, 4U, 16U, true}, {}};
    case 18U: // 8-byte 4x4 DXT1 blocks.
    case 51U:
    case 58U:
    case 59U:
    case 60U:
    case 61U:
        return {{4U, 4U, 8U, true}, {}};
    case 21U: // 64 bits per texel.
    case 26U:
    case 29U:
    case 32U:
    case 34U:
    case 37U:
        return {{1U, 1U, 8U, false}, {}};
    case 35U:
    case 38U:
        return {{1U, 1U, 16U, false}, {}};
    case 57U:
        return {{1U, 1U, 12U, false}, {}};
    default:
        return {{}, "Texture storage layout is not implemented for format " +
                           std::to_string(format)};
    }
}

} // namespace project360::gpu::xenos
