#pragma once

#include "gpu/xenos/pm4_draw.hpp"
#include "gpu/xenos/raster_state.hpp"
#include "gpu/xenos/shader_alu.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace project360::gpu::vulkan {

struct VulkanDeviceInfo {
    std::string name;
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t api_version{};
    std::uint32_t graphics_queue_family{};
};

struct VulkanPixelReadbackResult {
    std::array<std::uint8_t, 4> rgba{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

struct VulkanTriangleReadbackResult {
    std::array<std::uint8_t, 4> center_rgba{};
    std::array<std::uint8_t, 4> corner_rgba{};
    std::uint32_t green_pixel_count{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

struct VulkanTestVertex {
    std::array<float, 4> position{};
    std::array<float, 4> color{};
};

enum class VulkanContextStatus {
    ready,
    loader_unavailable,
    no_graphics_device,
    api_error,
};

class VulkanContext;

struct VulkanContextResult {
    std::unique_ptr<VulkanContext> context;
    VulkanContextStatus status{VulkanContextStatus::api_error};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && status == VulkanContextStatus::ready;
    }
};

// Owns one Vulkan instance and logical graphics device. Device entrypoints use
// Volk's loader dispatch, allowing the Vulkan runtime to stay optional.
class VulkanContext final {
  public:
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;
    ~VulkanContext();

    [[nodiscard]] static VulkanContextResult create();
    [[nodiscard]] const VulkanDeviceInfo& device_info() const noexcept;
    [[nodiscard]] VulkanPixelReadbackResult readback_clear_color_pixel() const;
    [[nodiscard]] VulkanTriangleReadbackResult render_offscreen_test_triangle() const;
    [[nodiscard]] VulkanTriangleReadbackResult render_offscreen_indexed_triangles(
        std::span<const VulkanTestVertex> vertices,
        std::span<const std::uint32_t> indices,
        std::optional<xenos::XenosScissorRectangle> scissor = std::nullopt,
        std::optional<xenos::XenosViewport> viewport = std::nullopt) const;
    [[nodiscard]] VulkanTriangleReadbackResult render_offscreen_xenos_triangles(
        std::span<const xenos::ShaderVertexOutputs> vertices,
        std::uint8_t color_interpolator,
        std::span<const std::uint32_t> indices,
        std::optional<xenos::XenosScissorRectangle> scissor = std::nullopt,
        std::optional<xenos::XenosViewport> viewport = std::nullopt) const;
    [[nodiscard]] VulkanTriangleReadbackResult render_offscreen_xenos_geometry(
        std::span<const xenos::ShaderVertexOutputs> vertices,
        std::uint8_t color_interpolator,
        const xenos::PrimitiveAssemblyResult& geometry,
        std::optional<xenos::XenosScissorRectangle> scissor = std::nullopt,
        std::optional<xenos::XenosViewport> viewport = std::nullopt) const;

  private:
    struct Impl;
    explicit VulkanContext(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace project360::gpu::vulkan
