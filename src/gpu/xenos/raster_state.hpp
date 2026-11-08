#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace project360::gpu::xenos {

struct XenosScissorRectangle {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};

    friend constexpr bool operator==(const XenosScissorRectangle&,
                                     const XenosScissorRectangle&) = default;
};

struct XenosScissorRectangleResult {
    XenosScissorRectangle rectangle{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Resolves the window and screen scissor registers, applies the optional window
// offset, intersects the bounds, and clamps them to the host render target.
[[nodiscard]] XenosScissorRectangleResult read_xenos_scissor_rectangle(
    std::span<const std::uint32_t> register_file, std::uint32_t target_width,
    std::uint32_t target_height);

} // namespace project360::gpu::xenos
