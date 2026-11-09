#include "gpu/xenos/texture_sampling.hpp"

#include "gpu/xenos/texture_fetch.hpp"
#include "gpu/xenos/texture_memory.hpp"
#include "memory/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace project360::gpu::xenos {
namespace {

struct TextureAxisIndexResult {
    std::uint32_t index{};
    std::string error;
};

TextureAxisIndexResult resolve_nearest_axis(float coordinate, std::uint32_t size,
                                            std::uint8_t clamp_mode) {
    if (!std::isfinite(coordinate)) {
        return {0U, "Texture coordinate must be finite"};
    }
    if (size == 0U) return {0U, "Texture axis has zero size"};

    double addressed = static_cast<double>(coordinate);
    switch (clamp_mode) {
    case 0U: { // Repeat.
        addressed -= std::floor(addressed);
        break;
    }
    case 1U: { // Mirrored repeat.
        const double segment = std::floor(addressed);
        const double fraction = addressed - segment;
        addressed = std::fmod(std::abs(segment), 2.0) >= 1.0 ? 1.0 - fraction : fraction;
        break;
    }
    case 2U: // Clamp to edge.
        addressed = std::clamp(addressed, 0.0, 1.0);
        break;
    case 3U: // Mirror once, then clamp to edge.
        addressed = std::clamp(std::abs(addressed), 0.0, 1.0);
        break;
    case 4U: // Clamp halfway to border.
    case 5U: // Mirror once halfway to border.
    case 6U: // Clamp to border.
    case 7U: // Mirror once to border.
        if (addressed < 0.0 || addressed > 1.0) {
            return {0U, "Out-of-range border and halfway clamp modes are unsupported"};
        }
        break;
    default:
        return {0U, "Texture clamp mode is unsupported"};
    }

    const double scaled = std::floor(addressed * static_cast<double>(size));
    const double last_index = static_cast<double>(size - 1U);
    return {static_cast<std::uint32_t>(std::clamp(scaled, 0.0, last_index)), {}};
}

} // namespace

TexturePointSampleResult sample_xenos_tiled_2d_texture_point(
    const TextureFetchConstant& fetch, std::uint32_t texel_x, std::uint32_t texel_y,
    const memory::GuestMemory& memory) {
    const auto storage = decode_texture_storage_layout(fetch.format);
    if (!storage) return {{}, storage.error};

    const auto block = read_xenos_tiled_2d_texture_block(fetch, texel_x, texel_y, memory);
    if (!block) return {{}, block.error};

    const auto decoded = decode_xenos_texture_block_rgba8(
        fetch.format, std::span<const std::byte>(block.bytes).first(block.byte_count));
    if (!decoded) return {{}, decoded.error};

    const auto interpreted =
        decode_xenos_texture_fetch_block_float(fetch.format, fetch, decoded);
    if (!interpreted) return {{}, interpreted.error};

    const auto local_x = texel_x % storage.layout.block_width_pixels;
    const auto local_y = texel_y % storage.layout.block_height_pixels;
    const auto local_index = static_cast<std::size_t>(local_y) *
                                 storage.layout.block_width_pixels +
                             local_x;
    if (local_index >= interpreted.texel_count) {
        return {{}, "Decoded texture block does not contain the requested texel"};
    }
    return {interpreted.texels[local_index], {}};
}

TexturePointSampleResult sample_xenos_tiled_2d_texture_normalized_point(
    const TextureFetchConstant& fetch, float u, float v, TextureSampleFilter filter,
    const memory::GuestMemory& memory) {
    if (filter != TextureSampleFilter::point) {
        return {{}, "Only point filtering is implemented for normalized texture samples"};
    }
    if (fetch.nearest_clamp_policy) {
        return {{}, "D3D/OpenGL nearest clamp policy is not implemented"};
    }
    if (fetch.dimension != TextureDataDimension::two_d_or_stacked || fetch.stacked) {
        return {{}, "Normalized point sampling requires a non-stacked 2D texture"};
    }

    const auto x = resolve_nearest_axis(u, fetch.width, fetch.clamp_modes[0]);
    if (!x.error.empty()) return {{}, x.error};
    const auto y = resolve_nearest_axis(v, fetch.height, fetch.clamp_modes[1]);
    if (!y.error.empty()) return {{}, y.error};
    return sample_xenos_tiled_2d_texture_point(fetch, x.index, y.index, memory);
}

} // namespace project360::gpu::xenos
