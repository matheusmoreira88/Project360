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

struct XenosViewport {
    float x{};
    float y{};
    float width{};
    float height{};
    bool flip_x{};
    bool flip_y{};
    bool remap_z_from_gl_clip_space{};

    friend constexpr bool operator==(const XenosViewport&, const XenosViewport&) = default;
};

struct XenosViewportResult {
    XenosViewport viewport{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Resolves the window and screen scissor registers, applies the optional window
// offset, intersects the bounds, and clamps them to the host render target.
[[nodiscard]] XenosScissorRectangleResult read_xenos_scissor_rectangle(
    std::span<const std::uint32_t> register_file, std::uint32_t target_width,
    std::uint32_t target_height);

// Resolves the clipping-enabled viewport subset used by the current Vulkan
// path. Z viewport transforms, user clip planes, and clipping-disabled draws fail.
[[nodiscard]] XenosViewportResult read_xenos_viewport(
    std::span<const std::uint32_t> register_file);

} // namespace project360::gpu::xenos
