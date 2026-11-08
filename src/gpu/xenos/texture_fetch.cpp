#include "gpu/xenos/texture_fetch.hpp"

#include <cstddef>

namespace project360::gpu::xenos {
namespace {

constexpr std::uint32_t kFetchConstantTypeMask = 0x3U;
constexpr std::uint32_t kTextureFetchConstantType = 0x2U;
constexpr std::size_t kFetchConstantRegisterBase = 0x4800U;
constexpr std::size_t kTextureFetchDwordCount = 6U;
constexpr std::size_t kTextureFetchSlotCount = 32U;

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

} // namespace project360::gpu::xenos
