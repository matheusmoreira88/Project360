#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace project360::gpu::xenos {

namespace register_index {
inline constexpr std::size_t rb_surface_info = 0x2000U;
inline constexpr std::size_t pa_sc_screen_scissor_tl = 0x200eU;
inline constexpr std::size_t pa_sc_screen_scissor_br = 0x200fU;
inline constexpr std::size_t pa_sc_window_offset = 0x2080U;
inline constexpr std::size_t pa_sc_window_scissor_tl = 0x2081U;
inline constexpr std::size_t pa_sc_window_scissor_br = 0x2082U;
inline constexpr std::size_t vgt_max_vtx_indx = 0x2100U;
inline constexpr std::size_t vgt_min_vtx_indx = 0x2101U;
inline constexpr std::size_t vgt_indx_offset = 0x2102U;
inline constexpr std::size_t vgt_multi_prim_ib_reset_indx = 0x2103U;
inline constexpr std::size_t vgt_dma_base = 0x21faU;
inline constexpr std::size_t vgt_dma_size = 0x21fbU;
inline constexpr std::size_t vgt_draw_initiator = 0x21fcU;
} // namespace register_index

struct XenosRegisterDescriptor {
    std::size_t index{};
    std::string_view name;
    std::uint32_t meaningful_bits_mask{};
};

// This initial map covers draw and vertex-index state only. Unlisted register
// addresses remain opaque in the generic PM4 register file.
[[nodiscard]] constexpr std::optional<XenosRegisterDescriptor> find_xenos_register(
    std::size_t index) noexcept {
    constexpr std::array<XenosRegisterDescriptor, 13> registers{{
        {register_index::rb_surface_info, "RB_SURFACE_INFO", 0xffff3fffU},
        {register_index::pa_sc_screen_scissor_tl, "PA_SC_SCREEN_SCISSOR_TL", 0x7fff7fffU},
        {register_index::pa_sc_screen_scissor_br, "PA_SC_SCREEN_SCISSOR_BR", 0x7fff7fffU},
        {register_index::pa_sc_window_offset, "PA_SC_WINDOW_OFFSET", 0x7fff7fffU},
        {register_index::pa_sc_window_scissor_tl, "PA_SC_WINDOW_SCISSOR_TL", 0xbfff3fffU},
        {register_index::pa_sc_window_scissor_br, "PA_SC_WINDOW_SCISSOR_BR", 0x3fff3fffU},
        {register_index::vgt_max_vtx_indx, "VGT_MAX_VTX_INDX", 0x00ffffffU},
        {register_index::vgt_min_vtx_indx, "VGT_MIN_VTX_INDX", 0x00ffffffU},
        {register_index::vgt_indx_offset, "VGT_INDX_OFFSET", 0x00ffffffU},
        {register_index::vgt_multi_prim_ib_reset_indx, "VGT_MULTI_PRIM_IB_RESET_INDX",
         0x00ffffffU},
        {register_index::vgt_dma_base, "VGT_DMA_BASE", 0xffffffffU},
        {register_index::vgt_dma_size, "VGT_DMA_SIZE", 0xc0ffffffU},
        {register_index::vgt_draw_initiator, "VGT_DRAW_INITIATOR", 0xffff1bffU},
    }};
    for (const auto& reg : registers) {
        if (reg.index == index) return reg;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::uint32_t normalize_xenos_register_write(
    std::size_t index, std::uint32_t value) noexcept {
    const auto descriptor = find_xenos_register(index);
    return descriptor ? value & descriptor->meaningful_bits_mask : value;
}

} // namespace project360::gpu::xenos
