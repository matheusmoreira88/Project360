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

struct TextureAxisCoordinateResult {
    double coordinate{};
    std::string error;
};

struct TextureAxisTapsResult {
    std::uint32_t first{};
    std::uint32_t second{};
    float weight{};
    std::string error;
};

TextureAxisCoordinateResult resolve_addressed_coordinate(float coordinate,
                                                          std::uint8_t clamp_mode) {
    if (!std::isfinite(coordinate)) {
        return {0.0, "Texture coordinate must be finite"};
    }

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
            return {0.0, "Out-of-range border and halfway clamp modes are unsupported"};
        }
        break;
    default:
        return {0.0, "Texture clamp mode is unsupported"};
    }
    return {addressed, {}};
}

TextureAxisIndexResult resolve_nearest_axis(float coordinate, std::uint32_t size,
                                            std::uint8_t clamp_mode) {
    if (size == 0U) return {0U, "Texture axis has zero size"};
    const auto addressed = resolve_addressed_coordinate(coordinate, clamp_mode);
    if (!addressed.error.empty()) return {0U, addressed.error};

    const double scaled = std::floor(addressed.coordinate * static_cast<double>(size));
    const double last_index = static_cast<double>(size - 1U);
    return {static_cast<std::uint32_t>(std::clamp(scaled, 0.0, last_index)), {}};
}

TextureAxisIndexResult address_linear_tap(std::int64_t tap, std::uint32_t size,
                                          std::uint8_t clamp_mode) {
    if (size == 0U) return {0U, "Texture axis has zero size"};
    const auto signed_size = static_cast<std::int64_t>(size);
    switch (clamp_mode) {
    case 0U: { // Repeat.
        auto wrapped = tap % signed_size;
        if (wrapped < 0) wrapped += signed_size;
        return {static_cast<std::uint32_t>(wrapped), {}};
    }
    case 1U: { // Mirrored repeat.
        const auto period = signed_size * 2;
        auto mirrored = tap % period;
        if (mirrored < 0) mirrored += period;
        if (mirrored >= signed_size) mirrored = period - 1 - mirrored;
        return {static_cast<std::uint32_t>(mirrored), {}};
    }
    case 2U: // Clamp to edge.
    case 3U: // Mirror once, then clamp to edge.
        return {static_cast<std::uint32_t>(std::clamp(tap, std::int64_t{0},
                                                      signed_size - 1)),
                {}};
    case 4U: // Clamp halfway to border.
    case 5U: // Mirror once halfway to border.
    case 6U: // Clamp to border.
    case 7U: // Mirror once to border.
        if (tap < 0 || tap >= signed_size) {
            return {0U, "Linear tap requires an unsupported border color"};
        }
        return {static_cast<std::uint32_t>(tap), {}};
    default:
        return {0U, "Texture clamp mode is unsupported"};
    }
}

TextureAxisTapsResult resolve_linear_axis(float coordinate, std::uint32_t size,
                                          std::uint8_t clamp_mode) {
    if (size == 0U) return {0U, 0U, 0.0F, "Texture axis has zero size"};
    const auto addressed = resolve_addressed_coordinate(coordinate, clamp_mode);
    if (!addressed.error.empty()) return {0U, 0U, 0.0F, addressed.error};

    const double texel_coordinate =
        addressed.coordinate * static_cast<double>(size) - 0.5;
    const double first_floor = std::floor(texel_coordinate);
    const auto first_tap = static_cast<std::int64_t>(first_floor);
    const auto first = address_linear_tap(first_tap, size, clamp_mode);
    if (!first.error.empty()) return {0U, 0U, 0.0F, first.error};
    const auto second = address_linear_tap(first_tap + 1, size, clamp_mode);
    if (!second.error.empty()) return {0U, 0U, 0.0F, second.error};

    const auto weight = static_cast<float>(texel_coordinate - first_floor);
    return {first.index, second.index, weight, {}};
}

TextureFloat4 lerp(TextureFloat4 first, TextureFloat4 second, float weight) {
    const float inverse_weight = 1.0F - weight;
    return {first.r * inverse_weight + second.r * weight,
            first.g * inverse_weight + second.g * weight,
            first.b * inverse_weight + second.b * weight,
            first.a * inverse_weight + second.a * weight};
}

} // namespace

TextureSampleResult sample_xenos_tiled_2d_texture_point(
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

TextureSampleResult sample_xenos_tiled_2d_texture_normalized(
    const TextureFetchConstant& fetch, float u, float v, TextureSampleFilter filter,
    const memory::GuestMemory& memory) {
    if (filter == TextureSampleFilter::point && fetch.nearest_clamp_policy) {
        return {{}, "D3D/OpenGL nearest clamp policy is not implemented"};
    }
    if (fetch.dimension != TextureDataDimension::two_d_or_stacked || fetch.stacked) {
        return {{}, "Normalized sampling requires a non-stacked 2D texture"};
    }

    if (filter == TextureSampleFilter::point) {
        const auto x = resolve_nearest_axis(u, fetch.width, fetch.clamp_modes[0]);
        if (!x.error.empty()) return {{}, x.error};
        const auto y = resolve_nearest_axis(v, fetch.height, fetch.clamp_modes[1]);
        if (!y.error.empty()) return {{}, y.error};
        return sample_xenos_tiled_2d_texture_point(fetch, x.index, y.index, memory);
    }
    if (filter != TextureSampleFilter::linear) {
        return {{}, "Texture sample filter is unsupported"};
    }
    if (fetch.number_format != 0U) {
        return {{}, "Linear filtering with integer texture number format is unsupported"};
    }

    const auto x = resolve_linear_axis(u, fetch.width, fetch.clamp_modes[0]);
    if (!x.error.empty()) return {{}, x.error};
    const auto y = resolve_linear_axis(v, fetch.height, fetch.clamp_modes[1]);
    if (!y.error.empty()) return {{}, y.error};

    const auto top_left =
        sample_xenos_tiled_2d_texture_point(fetch, x.first, y.first, memory);
    if (!top_left) return top_left;
    const auto top_right =
        sample_xenos_tiled_2d_texture_point(fetch, x.second, y.first, memory);
    if (!top_right) return top_right;
    const auto bottom_left =
        sample_xenos_tiled_2d_texture_point(fetch, x.first, y.second, memory);
    if (!bottom_left) return bottom_left;
    const auto bottom_right =
        sample_xenos_tiled_2d_texture_point(fetch, x.second, y.second, memory);
    if (!bottom_right) return bottom_right;

    const auto top = lerp(top_left.value, top_right.value, x.weight);
    const auto bottom = lerp(bottom_left.value, bottom_right.value, x.weight);
    return {lerp(top, bottom, y.weight), {}};
}

} // namespace project360::gpu::xenos
