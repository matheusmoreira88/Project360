#include "gpu/xenos/raster_state.hpp"

#include "gpu/xenos/registers.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace project360::gpu::xenos {
namespace {

std::int32_t sign_extend_15(std::uint32_t value) noexcept {
    value &= 0x7fffU;
    if ((value & 0x4000U) != 0U) value |= 0xffff8000U;
    return static_cast<std::int32_t>(value);
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

} // namespace project360::gpu::xenos
