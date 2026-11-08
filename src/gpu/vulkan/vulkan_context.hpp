#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace project360::gpu::vulkan {

struct VulkanDeviceInfo {
    std::string name;
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t api_version{};
    std::uint32_t graphics_queue_family{};
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

  private:
    struct Impl;
    explicit VulkanContext(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace project360::gpu::vulkan
