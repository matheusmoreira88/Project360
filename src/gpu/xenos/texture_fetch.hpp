#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace project360::gpu::xenos {

// Decoded metadata from a six-dword Xenos texture fetch constant. The raw
// words are retained because dimension-dependent fields and sampling behavior
// are not interpreted by this type yet.
struct TextureFetchConstant {
    std::array<std::uint32_t, 6> raw_words{};
    std::array<std::uint8_t, 4> sign_modes{};
    std::array<std::uint8_t, 3> clamp_modes{};
    std::uint16_t pitch{};
    std::uint8_t format{};
    std::uint8_t endian{};
    std::uint8_t request_size{};
    std::uint32_t base_address_bytes{};
    bool tiled{};
    bool stacked{};
    bool nearest_clamp_policy{};
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

// Decodes only fields whose locations are independent of texture dimension.
// Type values other than Xenos texture fetch constants are rejected.
[[nodiscard]] TextureFetchDecodeResult decode_texture_fetch_constant(
    const std::array<std::uint32_t, 6>& dwords);

// Resolves only requested texture slots from the shared FETCH_CONSTANT_0
// register bank. Texture slots contain six dwords; vertex slots use a separate
// two-dword interpretation of this same register space.
[[nodiscard]] TextureFetchConstantBankResult decode_texture_fetch_constant_bank(
    std::span<const std::uint32_t> register_file,
    std::span<const std::uint8_t> required_slots);

} // namespace project360::gpu::xenos
