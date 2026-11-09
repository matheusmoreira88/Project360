#pragma once

#include "gpu/xenos/texture_decode.hpp"

#include <cstdint>
#include <string>

namespace project360::memory {
class GuestMemory;
}

namespace project360::gpu::xenos {

struct TextureSampleResult {
    TextureFloat4 value{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

enum class TextureSampleFilter : std::uint8_t {
    point,
    linear
};

// Reads and decodes the texel at an integer base-level coordinate from a
// non-stacked 2D texture. This is a point lookup by texel index; it does not
// implement normalized UVs, filtering, mip selection or other dimensions.
[[nodiscard]] TextureSampleResult sample_xenos_2d_texture_point(
    const TextureFetchConstant& fetch, std::uint32_t texel_x, std::uint32_t texel_y,
    const memory::GuestMemory& memory);

// Resolves normalized UVs with descriptor clamp modes and the filter selected
// after shader-instruction overrides. Border/halfway modes outside [0,1] fail
// clearly; linear filtering fails at any tap that requires a border color.
[[nodiscard]] TextureSampleResult sample_xenos_2d_texture_normalized(
    const TextureFetchConstant& fetch, float u, float v, TextureSampleFilter filter,
    const memory::GuestMemory& memory);

} // namespace project360::gpu::xenos
