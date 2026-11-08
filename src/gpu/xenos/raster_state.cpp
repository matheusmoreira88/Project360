#include "gpu/xenos/raster_state.hpp"

#include "gpu/xenos/registers.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace project360::gpu::xenos {
namespace {

std::int32_t sign_extend_15(std::uint32_t value) noexcept {
    value &= 0x7fffU;
    if ((value & 0x4000U) != 0U) value |= 0xffff8000U;
    return static_cast<std::int32_t>(value);
}

float read_register_float(std::span<const std::uint32_t> register_file,
                          std::size_t index) noexcept {
    return std::bit_cast<float>(register_file[index]);
}

} // namespace

XenosScissorRectangleResult read_xenos_scissor_rectangle(
    std::span<const std::uint32_t> register_file, std::uint32_t target_width,
    std::uint32_t target_height) {
    constexpr auto last_register = register_index::pa_sc_window_scissor_br;
    if (target_width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        target_height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return {{}, "Xenos scissor target dimensions exceed signed coordinate limits"};
    }
    if (register_file.size() <= last_register) {
        return {{}, "Register file does not contain Xenos scissor state"};
    }

    const auto screen_tl = register_file[register_index::pa_sc_screen_scissor_tl];
    const auto screen_br = register_file[register_index::pa_sc_screen_scissor_br];
    std::int32_t left = static_cast<std::int32_t>(
        register_file[register_index::pa_sc_window_scissor_tl] & 0x3fffU);
    std::int32_t top = static_cast<std::int32_t>(
        (register_file[register_index::pa_sc_window_scissor_tl] >> 16U) & 0x3fffU);
    std::int32_t right = static_cast<std::int32_t>(
        register_file[register_index::pa_sc_window_scissor_br] & 0x3fffU);
    std::int32_t bottom = static_cast<std::int32_t>(
        (register_file[register_index::pa_sc_window_scissor_br] >> 16U) & 0x3fffU);

    constexpr std::uint32_t window_offset_disable_mask = 1U << 31U;
    if ((register_file[register_index::pa_sc_window_scissor_tl] &
         window_offset_disable_mask) == 0U) {
        const auto window_offset = register_file[register_index::pa_sc_window_offset];
        left += sign_extend_15(window_offset & 0x7fffU);
        top += sign_extend_15((window_offset >> 16U) & 0x7fffU);
        right += sign_extend_15(window_offset & 0x7fffU);
        bottom += sign_extend_15((window_offset >> 16U) & 0x7fffU);
    }

    left = std::max(left, sign_extend_15(screen_tl & 0x7fffU));
    top = std::max(top, sign_extend_15((screen_tl >> 16U) & 0x7fffU));
    right = std::min(right, sign_extend_15(screen_br & 0x7fffU));
    bottom = std::min(bottom, sign_extend_15((screen_br >> 16U) & 0x7fffU));

    left = std::clamp(left, 0, static_cast<std::int32_t>(target_width));
    top = std::clamp(top, 0, static_cast<std::int32_t>(target_height));
    right = std::clamp(std::max(right, left), left,
                       static_cast<std::int32_t>(target_width));
    bottom = std::clamp(std::max(bottom, top), top,
                        static_cast<std::int32_t>(target_height));

    return {{static_cast<std::uint32_t>(left), static_cast<std::uint32_t>(top),
             static_cast<std::uint32_t>(right - left),
             static_cast<std::uint32_t>(bottom - top)},
            {}};
}

XenosViewportResult read_xenos_viewport(std::span<const std::uint32_t> register_file) {
    constexpr auto last_register = register_index::pa_su_vtx_cntl;
    if (register_file.size() <= last_register) {
        return {{}, "Register file does not contain the Xenos viewport state"};
    }

    constexpr std::uint32_t clip_disabled_mask = 1U << 16U;
    constexpr std::uint32_t dx_clip_space_mask = 1U << 19U;
    constexpr std::uint32_t user_clip_enable_mask = 0x3fU;
    constexpr std::uint32_t user_clip_cull_only_mask = 1U << 17U;
    const auto clip_control = register_file[register_index::pa_cl_clip_cntl];
    if ((clip_control & clip_disabled_mask) != 0U) {
        return {{}, "Xenos viewport path does not support clipping-disabled draws"};
    }
    if ((clip_control & dx_clip_space_mask) == 0U) {
        return {{}, "Xenos viewport path requires Direct3D 0-to-W clip space"};
    }
    if ((clip_control & (user_clip_enable_mask | user_clip_cull_only_mask)) != 0U) {
        return {{}, "Xenos user clip planes are unsupported by the viewport path"};
    }

    constexpr std::uint32_t x_scale_enable_mask = 1U << 0U;
    constexpr std::uint32_t x_offset_enable_mask = 1U << 1U;
    constexpr std::uint32_t y_scale_enable_mask = 1U << 2U;
    constexpr std::uint32_t y_offset_enable_mask = 1U << 3U;
    constexpr std::uint32_t z_transform_enable_mask = (1U << 4U) | (1U << 5U);
    const auto vte_control = register_file[register_index::pa_cl_vte_cntl];
    if ((vte_control & z_transform_enable_mask) != 0U) {
        return {{}, "Xenos Z viewport scale/offset is not implemented"};
    }

    const auto scale_x = (vte_control & x_scale_enable_mask) != 0U
                             ? read_register_float(register_file,
                                                   register_index::pa_cl_vport_xscale)
                             : 1.0F;
    const auto scale_y = (vte_control & y_scale_enable_mask) != 0U
                             ? read_register_float(register_file,
                                                   register_index::pa_cl_vport_yscale)
                             : 1.0F;
    auto offset_x = (vte_control & x_offset_enable_mask) != 0U
                        ? read_register_float(register_file,
                                              register_index::pa_cl_vport_xoffset)
                        : 0.0F;
    auto offset_y = (vte_control & y_offset_enable_mask) != 0U
                        ? read_register_float(register_file,
                                              register_index::pa_cl_vport_yoffset)
                        : 0.0F;

    constexpr std::uint32_t window_offset_enable_mask = 1U << 16U;
    if ((register_file[register_index::pa_su_sc_mode_cntl] &
         window_offset_enable_mask) != 0U) {
        const auto window_offset = register_file[register_index::pa_sc_window_offset];
        offset_x += static_cast<float>(sign_extend_15(window_offset & 0x7fffU));
        offset_y += static_cast<float>(sign_extend_15((window_offset >> 16U) & 0x7fffU));
    }

    constexpr std::uint32_t pixel_center_mask = 1U;
    if ((register_file[register_index::pa_su_vtx_cntl] & pixel_center_mask) == 0U) {
        offset_x += 0.5F;
        offset_y += 0.5F;
    }

    if (!std::isfinite(scale_x) || !std::isfinite(scale_y) ||
        !std::isfinite(offset_x) || !std::isfinite(offset_y)) {
        return {{}, "Xenos viewport contains non-finite scale or offset values"};
    }
    if (scale_x == 0.0F || scale_y == 0.0F) {
        return {{}, "Xenos zero-sized viewport axes are unsupported"};
    }

    const auto extent_x = std::abs(scale_x);
    const auto extent_y = std::abs(scale_y);
    XenosViewport viewport{offset_x - extent_x, offset_y - extent_y,
                           extent_x * 2.0F, extent_y * 2.0F,
                           std::signbit(scale_x), std::signbit(scale_y)};
    if (!std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
        !std::isfinite(viewport.width) || !std::isfinite(viewport.height)) {
        return {{}, "Xenos viewport conversion exceeds finite host coordinates"};
    }
    return {viewport, {}};
}

} // namespace project360::gpu::xenos
