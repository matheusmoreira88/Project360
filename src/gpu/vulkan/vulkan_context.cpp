#include "gpu/vulkan/vulkan_context.hpp"

#include <volk.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace project360::gpu::vulkan {

struct VulkanContext::Impl {
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue graphics_queue{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VulkanDeviceInfo info;
    std::mutex graphics_queue_mutex;

    ~Impl() {
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }
};

namespace {

VulkanContextResult failure(VulkanContextStatus status, std::string message) {
    return {nullptr, status, std::move(message)};
}

std::string result_message(const char* operation, VkResult result) {
    return std::string(operation) + " failed with VkResult " + std::to_string(result);
}

struct ClearResources {
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    VkImage image{VK_NULL_HANDLE};
    VkImageView image_view{VK_NULL_HANDLE};
    VkDeviceMemory image_memory{VK_NULL_HANDLE};
    VkBuffer staging_buffer{VK_NULL_HANDLE};
    VkDeviceMemory staging_memory{VK_NULL_HANDLE};
    VkBuffer vertex_buffer{VK_NULL_HANDLE};
    VkDeviceMemory vertex_memory{VK_NULL_HANDLE};
    VkBuffer index_buffer{VK_NULL_HANDLE};
    VkDeviceMemory index_memory{VK_NULL_HANDLE};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkShaderModule vertex_shader{VK_NULL_HANDLE};
    VkShaderModule fragment_shader{VK_NULL_HANDLE};
    bool submission_pending{};

    ~ClearResources() {
        if (submission_pending && queue != VK_NULL_HANDLE) {
            static_cast<void>(vkQueueWaitIdle(queue));
        }
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, fence, nullptr);
        }
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        if (image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, image_view, nullptr);
        }
        if (render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, render_pass, nullptr);
        }
        if (vertex_shader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, vertex_shader, nullptr);
        }
        if (fragment_shader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, fragment_shader, nullptr);
        }
        if (command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, command_pool, nullptr);
        }
        if (staging_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, staging_buffer, nullptr);
        }
        if (vertex_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, vertex_buffer, nullptr);
        }
        if (index_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, index_buffer, nullptr);
        }
        if (staging_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, staging_memory, nullptr);
        }
        if (vertex_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, vertex_memory, nullptr);
        }
        if (index_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, index_memory, nullptr);
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
        }
        if (image_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, image_memory, nullptr);
        }
    }
};

bool read_spirv(const char* filename, std::vector<std::uint32_t>& words, std::string& error) {
    const std::string path = std::string(PROJECT360_VULKAN_SHADER_DIR) + "/" + filename;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        error = "Could not open Vulkan test shader: " + path;
        return false;
    }
    const auto end = stream.tellg();
    if (end <= 0 || (static_cast<std::streamoff>(end) % 4) != 0) {
        error = "Vulkan test shader has an invalid SPIR-V byte length: " + path;
        return false;
    }
    words.resize(static_cast<std::size_t>(end) / sizeof(std::uint32_t));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(end));
    if (!stream) {
        error = "Could not read complete Vulkan test shader: " + path;
        words.clear();
        return false;
    }
    return true;
}

bool select_memory_type(const VkPhysicalDeviceMemoryProperties& properties,
                        std::uint32_t allowed_types, VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred, std::uint32_t& selected_type,
                        VkMemoryPropertyFlags& selected_flags) {
    int best_score = -1;
    for (std::uint32_t i = 0U; i < properties.memoryTypeCount; ++i) {
        if ((allowed_types & (1U << i)) == 0U) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) {
            continue;
        }
        int score = 0;
        for (auto bits = preferred; bits != 0U; bits &= bits - 1U) {
            if ((flags & (bits & (~bits + 1U))) != 0U) {
                ++score;
            }
        }
        if (score > best_score) {
            best_score = score;
            selected_type = i;
            selected_flags = flags;
        }
    }
    return best_score >= 0;
}

bool create_host_uploaded_buffer(VkDevice device,
                                const VkPhysicalDeviceMemoryProperties& memory_properties,
                                std::span<const std::byte> contents, VkBufferUsageFlags usage,
                                VkBuffer& buffer, VkDeviceMemory& memory,
                                std::string& error) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = static_cast<VkDeviceSize>(contents.size());
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    auto result = vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
    if (result != VK_SUCCESS) {
        error = result_message("vkCreateBuffer(upload)", result);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    std::uint32_t memory_type = 0U;
    VkMemoryPropertyFlags memory_flags = 0U;
    if (!select_memory_type(memory_properties, requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory_type, memory_flags)) {
        error = "Vulkan device has no host-visible memory for a draw buffer";
        return false;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(device, &allocation, nullptr, &memory);
    if (result != VK_SUCCESS) {
        error = result_message("vkAllocateMemory(upload)", result);
        return false;
    }
    result = vkBindBufferMemory(device, buffer, memory, 0U);
    if (result != VK_SUCCESS) {
        error = result_message("vkBindBufferMemory(upload)", result);
        return false;
    }
    void* mapped = nullptr;
    result = vkMapMemory(device, memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
    if (result != VK_SUCCESS) {
        error = result_message("vkMapMemory(upload)", result);
        return false;
    }
    std::memcpy(mapped, contents.data(), contents.size());
    if ((memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = memory;
        range.size = VK_WHOLE_SIZE;
        result = vkFlushMappedMemoryRanges(device, 1U, &range);
        if (result != VK_SUCCESS) {
            vkUnmapMemory(device, memory);
            error = result_message("vkFlushMappedMemoryRanges(upload)", result);
            return false;
        }
    }
    vkUnmapMemory(device, memory);
    return true;
}

bool find_graphics_queue(VkPhysicalDevice physical_device, std::uint32_t& family_index) {
    std::uint32_t family_count = 0U;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
    if (family_count == 0U) {
        return false;
    }

    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
    const auto returned_count = std::min<std::size_t>(family_count, families.size());
    const auto graphics_family = std::find_if(
        families.begin(), families.begin() + static_cast<std::ptrdiff_t>(returned_count),
        [](const VkQueueFamilyProperties& family) {
            return family.queueCount > 0U && (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U;
        });
    if (graphics_family == families.end()) {
        return false;
    }

    family_index = static_cast<std::uint32_t>(std::distance(families.begin(), graphics_family));
    return true;
}

} // namespace

VulkanContext::VulkanContext(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

VulkanContext::~VulkanContext() = default;

VulkanContextResult VulkanContext::create() {
    const auto loader_result = volkInitialize();
    if (loader_result != VK_SUCCESS) {
        return failure(VulkanContextStatus::loader_unavailable,
                       "Vulkan loader could not be initialized: " + std::to_string(loader_result));
    }

    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Project360";
    application_info.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application_info.pEngineName = "Project360";
    application_info.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;

    auto impl = std::make_unique<Impl>();
    auto result = vkCreateInstance(&instance_info, nullptr, &impl->instance);
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
        return failure(VulkanContextStatus::no_graphics_device,
                       "No Vulkan 1.0 compatible driver is available");
    }
    if (result != VK_SUCCESS) {
        return failure(VulkanContextStatus::api_error, result_message("vkCreateInstance", result));
    }
    volkLoadInstance(impl->instance);

    std::uint32_t physical_device_count = 0U;
    result = vkEnumeratePhysicalDevices(impl->instance, &physical_device_count, nullptr);
    if (result != VK_SUCCESS) {
        return failure(VulkanContextStatus::api_error,
                       result_message("vkEnumeratePhysicalDevices", result));
    }
    if (physical_device_count == 0U) {
        return failure(VulkanContextStatus::no_graphics_device,
                       "The Vulkan loader found no physical devices");
    }

    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    result =
        vkEnumeratePhysicalDevices(impl->instance, &physical_device_count, physical_devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return failure(VulkanContextStatus::api_error,
                       result_message("vkEnumeratePhysicalDevices", result));
    }

    struct Candidate {
        VkPhysicalDevice physical_device{VK_NULL_HANDLE};
        VkPhysicalDeviceProperties properties{};
        std::uint32_t graphics_queue_family{};
        int score{-1};
    };
    Candidate selected;
    const auto enumerated_count =
        std::min<std::size_t>(physical_device_count, physical_devices.size());
    for (std::size_t i = 0U; i < enumerated_count; ++i) {
        Candidate candidate;
        candidate.physical_device = physical_devices[i];
        vkGetPhysicalDeviceProperties(candidate.physical_device, &candidate.properties);
        if (!find_graphics_queue(candidate.physical_device, candidate.graphics_queue_family)) {
            continue;
        }
        candidate.score =
            candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 2
            : candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                        : 0;
        if (candidate.score > selected.score) {
            selected = candidate;
        }
    }
    if (selected.physical_device == VK_NULL_HANDLE) {
        return failure(VulkanContextStatus::no_graphics_device,
                       "No Vulkan physical device exposes a graphics queue");
    }

    constexpr float queue_priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = selected.graphics_queue_family;
    queue_info.queueCount = 1U;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1U;
    device_info.pQueueCreateInfos = &queue_info;

    impl->physical_device = selected.physical_device;
    result = vkCreateDevice(impl->physical_device, &device_info, nullptr, &impl->device);
    if (result != VK_SUCCESS) {
        return failure(VulkanContextStatus::api_error, result_message("vkCreateDevice", result));
    }
    vkGetDeviceQueue(impl->device, selected.graphics_queue_family, 0U, &impl->graphics_queue);
    if (impl->graphics_queue == VK_NULL_HANDLE) {
        return failure(VulkanContextStatus::api_error,
                       "vkGetDeviceQueue returned a null graphics queue");
    }

    impl->info.name = selected.properties.deviceName;
    impl->info.vendor_id = selected.properties.vendorID;
    impl->info.device_id = selected.properties.deviceID;
    impl->info.api_version = selected.properties.apiVersion;
    impl->info.graphics_queue_family = selected.graphics_queue_family;
    vkGetPhysicalDeviceMemoryProperties(impl->physical_device, &impl->memory_properties);

    auto context = std::unique_ptr<VulkanContext>(new VulkanContext(std::move(impl)));
    return {std::move(context), VulkanContextStatus::ready, {}};
}

const VulkanDeviceInfo& VulkanContext::device_info() const noexcept {
    return impl_->info;
}

VulkanPixelReadbackResult VulkanContext::readback_clear_color_pixel() const {
    std::lock_guard queue_lock(impl_->graphics_queue_mutex);

    constexpr std::uint32_t image_width = 16U;
    constexpr std::uint32_t image_height = 16U;
    constexpr VkDeviceSize image_byte_count =
        static_cast<VkDeviceSize>(image_width) * image_height * 4U;
    constexpr std::uint32_t pixel_x = image_width / 2U;
    constexpr std::uint32_t pixel_y = image_height / 2U;

    VkImageFormatProperties format_properties{};
    auto result = vkGetPhysicalDeviceImageFormatProperties(
        impl_->physical_device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0U, &format_properties);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkGetPhysicalDeviceImageFormatProperties", result)};
    }
    if (format_properties.maxExtent.width < image_width ||
        format_properties.maxExtent.height < image_height) {
        return {{}, "Vulkan device cannot create the requested offscreen image extent"};
    }

    ClearResources resources;
    resources.device = impl_->device;
    resources.queue = impl_->graphics_queue;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {image_width, image_height, 1U};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = 1U;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(impl_->device, &image_info, nullptr, &resources.image);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkCreateImage", result)};
    }

    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(impl_->device, resources.image, &image_requirements);
    std::uint32_t image_memory_type = 0U;
    VkMemoryPropertyFlags image_memory_flags = 0U;
    if (!select_memory_type(impl_->memory_properties, image_requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0U, image_memory_type,
                            image_memory_flags)) {
        return {{}, "Vulkan device has no device-local memory type for the offscreen image"};
    }
    VkMemoryAllocateInfo image_allocation{};
    image_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_allocation.allocationSize = image_requirements.size;
    image_allocation.memoryTypeIndex = image_memory_type;
    result = vkAllocateMemory(impl_->device, &image_allocation, nullptr, &resources.image_memory);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkAllocateMemory(image)", result)};
    }
    result = vkBindImageMemory(impl_->device, resources.image, resources.image_memory, 0U);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkBindImageMemory", result)};
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = image_byte_count;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(impl_->device, &buffer_info, nullptr, &resources.staging_buffer);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkCreateBuffer", result)};
    }

    VkMemoryRequirements staging_requirements{};
    vkGetBufferMemoryRequirements(impl_->device, resources.staging_buffer, &staging_requirements);
    std::uint32_t staging_memory_type = 0U;
    VkMemoryPropertyFlags staging_memory_flags = 0U;
    if (!select_memory_type(impl_->memory_properties, staging_requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_memory_type,
                            staging_memory_flags)) {
        return {{}, "Vulkan device has no host-visible staging memory type"};
    }
    VkMemoryAllocateInfo staging_allocation{};
    staging_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    staging_allocation.allocationSize = staging_requirements.size;
    staging_allocation.memoryTypeIndex = staging_memory_type;
    result =
        vkAllocateMemory(impl_->device, &staging_allocation, nullptr, &resources.staging_memory);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkAllocateMemory(staging)", result)};
    }
    result =
        vkBindBufferMemory(impl_->device, resources.staging_buffer, resources.staging_memory, 0U);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkBindBufferMemory", result)};
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = impl_->info.graphics_queue_family;
    result = vkCreateCommandPool(impl_->device, &pool_info, nullptr, &resources.command_pool);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkCreateCommandPool", result)};
    }

    VkCommandBufferAllocateInfo command_allocation{};
    command_allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_allocation.commandPool = resources.command_pool;
    command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocation.commandBufferCount = 1U;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(impl_->device, &command_allocation, &command_buffer);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkAllocateCommandBuffers", result)};
    }

    VkCommandBufferBeginInfo command_begin{};
    command_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer, &command_begin);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkBeginCommandBuffer", result)};
    }

    VkImageMemoryBarrier image_barrier{};
    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.srcAccessMask = 0U;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = resources.image;
    image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                         &image_barrier);

    constexpr VkClearColorValue clear_color{{1.0F, 0.0F, 1.0F, 1.0F}};
    constexpr VkImageSubresourceRange image_range{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
    vkCmdClearColorImage(command_buffer, resources.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear_color, 1U, &image_range);

    image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                         &image_barrier);

    VkBufferImageCopy image_copy{};
    image_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U};
    image_copy.imageExtent = {image_width, image_height, 1U};
    vkCmdCopyImageToBuffer(command_buffer, resources.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           resources.staging_buffer, 1U, &image_copy);

    VkBufferMemoryBarrier buffer_barrier{};
    buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.buffer = resources.staging_buffer;
    buffer_barrier.offset = 0U;
    buffer_barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0U, 0U, nullptr, 1U, &buffer_barrier, 0U, nullptr);

    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkEndCommandBuffer", result)};
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(impl_->device, &fence_info, nullptr, &resources.fence);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkCreateFence", result)};
    }
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1U;
    submit_info.pCommandBuffers = &command_buffer;
    result = vkQueueSubmit(impl_->graphics_queue, 1U, &submit_info, resources.fence);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkQueueSubmit", result)};
    }
    resources.submission_pending = true;
    result = vkWaitForFences(impl_->device, 1U, &resources.fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkWaitForFences", result)};
    }
    resources.submission_pending = false;

    void* mapped_memory = nullptr;
    result =
        vkMapMemory(impl_->device, resources.staging_memory, 0U, VK_WHOLE_SIZE, 0U, &mapped_memory);
    if (result != VK_SUCCESS) {
        return {{}, result_message("vkMapMemory", result)};
    }
    if ((staging_memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        VkMappedMemoryRange mapped_range{};
        mapped_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mapped_range.memory = resources.staging_memory;
        mapped_range.offset = 0U;
        mapped_range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(impl_->device, 1U, &mapped_range);
        if (result != VK_SUCCESS) {
            vkUnmapMemory(impl_->device, resources.staging_memory);
            return {{}, result_message("vkInvalidateMappedMemoryRanges", result)};
        }
    }

    const auto* pixel = static_cast<const std::uint8_t*>(mapped_memory) +
                        (static_cast<std::size_t>(pixel_y) * image_width + pixel_x) * 4U;
    VulkanPixelReadbackResult readback;
    std::copy_n(pixel, readback.rgba.size(), readback.rgba.begin());
    vkUnmapMemory(impl_->device, resources.staging_memory);
    return readback;
}

VulkanTriangleReadbackResult VulkanContext::render_offscreen_test_triangle() const {
    constexpr std::array<VulkanTestVertex, 3> vertices{{
        {{{-0.8F, -0.8F, 0.0F, 1.0F}}, {{0.0F, 1.0F, 0.0F, 1.0F}}},
        {{{0.8F, -0.8F, 0.0F, 1.0F}}, {{0.0F, 1.0F, 0.0F, 1.0F}}},
        {{{0.0F, 0.8F, 0.0F, 1.0F}}, {{0.0F, 1.0F, 0.0F, 1.0F}}},
    }};
    constexpr std::array<std::uint32_t, 3> indices{0U, 1U, 2U};
    return render_offscreen_indexed_triangles(vertices, indices);
}

VulkanTriangleReadbackResult VulkanContext::render_offscreen_indexed_triangles(
    std::span<const VulkanTestVertex> vertices,
    std::span<const std::uint32_t> indices) const {
    constexpr std::size_t maximum_vertices = 65'536U;
    constexpr std::size_t maximum_indices = 196'608U;
    static_assert(sizeof(VulkanTestVertex) == 8U * sizeof(float));
    static_assert(offsetof(VulkanTestVertex, color) == 4U * sizeof(float));
    if (vertices.empty() || indices.empty() || indices.size() % 3U != 0U) {
        return {{}, {}, 0U, "Indexed triangle draw requires vertices and complete triangles"};
    }
    if (vertices.size() > maximum_vertices || indices.size() > maximum_indices ||
        indices.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {{}, {}, 0U, "Indexed triangle draw exceeds the diagnostic buffer budget"};
    }
    for (const auto index : indices) {
        if (index >= vertices.size()) {
            return {{}, {}, 0U, "Indexed triangle references a missing vertex"};
        }
    }
    for (const auto& vertex : vertices) {
        for (const auto value : vertex.position) {
            if (!std::isfinite(value)) {
                return {{}, {}, 0U, "Indexed triangle has a non-finite vertex position"};
            }
        }
        for (const auto value : vertex.color) {
            if (!std::isfinite(value)) {
                return {{}, {}, 0U, "Indexed triangle has a non-finite vertex color"};
            }
        }
    }
    std::lock_guard queue_lock(impl_->graphics_queue_mutex);

    constexpr std::uint32_t image_width = 16U;
    constexpr std::uint32_t image_height = 16U;
    constexpr std::size_t pixel_size = 4U;
    constexpr VkDeviceSize image_byte_count =
        static_cast<VkDeviceSize>(image_width) * image_height * pixel_size;
    constexpr VkImageSubresourceRange image_range{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};

    VkImageFormatProperties format_properties{};
    auto result = vkGetPhysicalDeviceImageFormatProperties(
        impl_->physical_device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0U,
        &format_properties);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkGetPhysicalDeviceImageFormatProperties", result)};
    }
    if (format_properties.maxExtent.width < image_width ||
        format_properties.maxExtent.height < image_height) {
        return {{}, {}, 0U, "Vulkan device cannot create the requested triangle image extent"};
    }

    ClearResources resources;
    resources.device = impl_->device;
    resources.queue = impl_->graphics_queue;

    std::string upload_error;
    const auto vertex_bytes = std::as_bytes(vertices);
    if (!create_host_uploaded_buffer(impl_->device, impl_->memory_properties, vertex_bytes,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, resources.vertex_buffer,
                                     resources.vertex_memory, upload_error)) {
        return {{}, {}, 0U, std::move(upload_error)};
    }
    const auto index_bytes = std::as_bytes(indices);
    if (!create_host_uploaded_buffer(impl_->device, impl_->memory_properties, index_bytes,
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, resources.index_buffer,
                                     resources.index_memory, upload_error)) {
        return {{}, {}, 0U, std::move(upload_error)};
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {image_width, image_height, 1U};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = 1U;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(impl_->device, &image_info, nullptr, &resources.image);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreateImage(triangle)", result)};
    }

    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(impl_->device, resources.image, &image_requirements);
    std::uint32_t image_memory_type = 0U;
    VkMemoryPropertyFlags image_memory_flags = 0U;
    if (!select_memory_type(impl_->memory_properties, image_requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0U, image_memory_type,
                            image_memory_flags)) {
        return {{}, {}, 0U, "Vulkan device has no device-local memory for the triangle image"};
    }
    VkMemoryAllocateInfo image_allocation{};
    image_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_allocation.allocationSize = image_requirements.size;
    image_allocation.memoryTypeIndex = image_memory_type;
    result = vkAllocateMemory(impl_->device, &image_allocation, nullptr, &resources.image_memory);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkAllocateMemory(triangle image)", result)};
    }
    result = vkBindImageMemory(impl_->device, resources.image, resources.image_memory, 0U);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkBindImageMemory(triangle)", result)};
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = image_byte_count;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(impl_->device, &buffer_info, nullptr, &resources.staging_buffer);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreateBuffer(triangle staging)", result)};
    }
    VkMemoryRequirements staging_requirements{};
    vkGetBufferMemoryRequirements(impl_->device, resources.staging_buffer, &staging_requirements);
    std::uint32_t staging_memory_type = 0U;
    VkMemoryPropertyFlags staging_memory_flags = 0U;
    if (!select_memory_type(impl_->memory_properties, staging_requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_memory_type,
                            staging_memory_flags)) {
        return {{}, {}, 0U, "Vulkan device has no host-visible triangle staging memory"};
    }
    VkMemoryAllocateInfo staging_allocation{};
    staging_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    staging_allocation.allocationSize = staging_requirements.size;
    staging_allocation.memoryTypeIndex = staging_memory_type;
    result =
        vkAllocateMemory(impl_->device, &staging_allocation, nullptr, &resources.staging_memory);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkAllocateMemory(triangle staging)", result)};
    }
    result = vkBindBufferMemory(impl_->device, resources.staging_buffer, resources.staging_memory,
                                0U);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkBindBufferMemory(triangle staging)", result)};
    }

    std::vector<std::uint32_t> vertex_words;
    std::vector<std::uint32_t> fragment_words;
    std::string shader_error;
    if (!read_spirv("offscreen_triangle.vert.spv", vertex_words, shader_error) ||
        !read_spirv("offscreen_triangle.frag.spv", fragment_words, shader_error)) {
        return {{}, {}, 0U, std::move(shader_error)};
    }
    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = vertex_words.size() * sizeof(std::uint32_t);
    shader_info.pCode = vertex_words.data();
    result = vkCreateShaderModule(impl_->device, &shader_info, nullptr, &resources.vertex_shader);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreateShaderModule(vertex)", result)};
    }
    shader_info.codeSize = fragment_words.size() * sizeof(std::uint32_t);
    shader_info.pCode = fragment_words.data();
    result = vkCreateShaderModule(impl_->device, &shader_info, nullptr, &resources.fragment_shader);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreateShaderModule(fragment)", result)};
    }

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_reference{0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1U;
    subpass.pColorAttachments = &color_reference;
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1U;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1U;
    render_pass_info.pSubpasses = &subpass;
    result = vkCreateRenderPass(impl_->device, &render_pass_info, nullptr, &resources.render_pass);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreateRenderPass(triangle)", result)};
    }

    VkImageViewCreateInfo image_view_info{};
    image_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    image_view_info.image = resources.image;
    image_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    image_view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_view_info.subresourceRange = image_range;
    result = vkCreateImageView(impl_->device, &image_view_info, nullptr, &resources.image_view);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreateImageView(triangle)", result)};
    }

    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = resources.render_pass;
    framebuffer_info.attachmentCount = 1U;
    framebuffer_info.pAttachments = &resources.image_view;
    framebuffer_info.width = image_width;
    framebuffer_info.height = image_height;
    framebuffer_info.layers = 1U;
    result = vkCreateFramebuffer(impl_->device, &framebuffer_info, nullptr, &resources.framebuffer);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreateFramebuffer(triangle)", result)};
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    result = vkCreatePipelineLayout(impl_->device, &layout_info, nullptr,
                                    &resources.pipeline_layout);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreatePipelineLayout(triangle)", result)};
    }
    const std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0U,
         VK_SHADER_STAGE_VERTEX_BIT, resources.vertex_shader, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0U,
         VK_SHADER_STAGE_FRAGMENT_BIT, resources.fragment_shader, "main", nullptr},
    }};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    const VkVertexInputBindingDescription vertex_binding{
        0U, static_cast<std::uint32_t>(sizeof(VulkanTestVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 2> vertex_attributes{{
        {0U, 0U, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(VulkanTestVertex, position))},
        {1U, 0U, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(VulkanTestVertex, color))},
    }};
    vertex_input.vertexBindingDescriptionCount = 1U;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(vertex_attributes.size());
    vertex_input.pVertexAttributeDescriptions = vertex_attributes.data();
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1U;
    viewport_state.scissorCount = 1U;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1U;
    blend.pAttachments = &blend_attachment;
    const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                      VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = static_cast<std::uint32_t>(shader_stages.size());
    pipeline_info.pStages = shader_stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = resources.pipeline_layout;
    pipeline_info.renderPass = resources.render_pass;
    pipeline_info.subpass = 0U;
    result = vkCreateGraphicsPipelines(impl_->device, VK_NULL_HANDLE, 1U, &pipeline_info, nullptr,
                                       &resources.pipeline);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreateGraphicsPipelines(triangle)", result)};
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = impl_->info.graphics_queue_family;
    result = vkCreateCommandPool(impl_->device, &pool_info, nullptr, &resources.command_pool);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreateCommandPool(triangle)", result)};
    }
    VkCommandBufferAllocateInfo command_allocation{};
    command_allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_allocation.commandPool = resources.command_pool;
    command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocation.commandBufferCount = 1U;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(impl_->device, &command_allocation, &command_buffer);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkAllocateCommandBuffers(triangle)", result)};
    }
    VkCommandBufferBeginInfo command_begin{};
    command_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer, &command_begin);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkBeginCommandBuffer(triangle)", result)};
    }
    std::array<VkBufferMemoryBarrier, 2> upload_barriers{};
    upload_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    upload_barriers[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    upload_barriers[0].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    upload_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    upload_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    upload_barriers[0].buffer = resources.vertex_buffer;
    upload_barriers[0].size = VK_WHOLE_SIZE;
    upload_barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    upload_barriers[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    upload_barriers[1].dstAccessMask = VK_ACCESS_INDEX_READ_BIT;
    upload_barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    upload_barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    upload_barriers[1].buffer = resources.index_buffer;
    upload_barriers[1].size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0U, 0U, nullptr,
                         static_cast<std::uint32_t>(upload_barriers.size()),
                         upload_barriers.data(), 0U, nullptr);
    VkClearValue clear_value{};
    clear_value.color = {{1.0F, 0.0F, 1.0F, 1.0F}};
    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = resources.render_pass;
    render_begin.framebuffer = resources.framebuffer;
    render_begin.renderArea.extent = {image_width, image_height};
    render_begin.clearValueCount = 1U;
    render_begin.pClearValues = &clear_value;
    vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(image_width),
                              static_cast<float>(image_height), 0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, {image_width, image_height}};
    vkCmdSetViewport(command_buffer, 0U, 1U, &viewport);
    vkCmdSetScissor(command_buffer, 0U, 1U, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, resources.pipeline);
    constexpr VkDeviceSize vertex_offset = 0U;
    vkCmdBindVertexBuffers(command_buffer, 0U, 1U, &resources.vertex_buffer, &vertex_offset);
    vkCmdBindIndexBuffer(command_buffer, resources.index_buffer, 0U, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(command_buffer, static_cast<std::uint32_t>(indices.size()), 1U, 0U, 0, 0U);
    vkCmdEndRenderPass(command_buffer);

    VkImageMemoryBarrier image_barrier{};
    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = resources.image;
    image_barrier.subresourceRange = image_range;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                         &image_barrier);
    VkBufferImageCopy image_copy{};
    image_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U};
    image_copy.imageExtent = {image_width, image_height, 1U};
    vkCmdCopyImageToBuffer(command_buffer, resources.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           resources.staging_buffer, 1U, &image_copy);
    VkBufferMemoryBarrier buffer_barrier{};
    buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.buffer = resources.staging_buffer;
    buffer_barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0U, 0U, nullptr, 1U, &buffer_barrier, 0U, nullptr);
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkEndCommandBuffer(triangle)", result)};
    }
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(impl_->device, &fence_info, nullptr, &resources.fence);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkCreateFence(triangle)", result)};
    }
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1U;
    submit_info.pCommandBuffers = &command_buffer;
    result = vkQueueSubmit(impl_->graphics_queue, 1U, &submit_info, resources.fence);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkQueueSubmit(triangle)", result)};
    }
    resources.submission_pending = true;
    result = vkWaitForFences(impl_->device, 1U, &resources.fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkWaitForFences(triangle)", result)};
    }
    resources.submission_pending = false;
    void* mapped_memory = nullptr;
    result = vkMapMemory(impl_->device, resources.staging_memory, 0U, VK_WHOLE_SIZE, 0U,
                         &mapped_memory);
    if (result != VK_SUCCESS) {
        return {{}, {}, 0U, result_message("vkMapMemory(triangle)", result)};
    }
    if ((staging_memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
        VkMappedMemoryRange mapped_range{};
        mapped_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mapped_range.memory = resources.staging_memory;
        mapped_range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(impl_->device, 1U, &mapped_range);
        if (result != VK_SUCCESS) {
            vkUnmapMemory(impl_->device, resources.staging_memory);
            return {{}, {}, 0U, result_message("vkInvalidateMappedMemoryRanges(triangle)", result)};
        }
    }

    VulkanTriangleReadbackResult readback;
    const auto* pixels = static_cast<const std::uint8_t*>(mapped_memory);
    const auto copy_pixel = [&](std::uint32_t x, std::uint32_t y,
                                std::array<std::uint8_t, 4>& destination) {
        const auto offset = (static_cast<std::size_t>(y) * image_width + x) * pixel_size;
        std::copy_n(pixels + offset, pixel_size, destination.begin());
    };
    copy_pixel(image_width / 2U, image_height / 2U, readback.center_rgba);
    copy_pixel(0U, 0U, readback.corner_rgba);
    for (std::uint32_t y = 0U; y < image_height; ++y) {
        for (std::uint32_t x = 0U; x < image_width; ++x) {
            std::array<std::uint8_t, 4> pixel{};
            copy_pixel(x, y, pixel);
            if (pixel == std::array<std::uint8_t, 4>{0U, 255U, 0U, 255U}) {
                ++readback.green_pixel_count;
            }
        }
    }
    vkUnmapMemory(impl_->device, resources.staging_memory);
    return readback;
}

VulkanTriangleReadbackResult VulkanContext::render_offscreen_xenos_triangles(
    std::span<const xenos::ShaderVertexOutputs> vertices,
    std::uint8_t color_interpolator,
    std::span<const std::uint32_t> indices) const {
    constexpr std::size_t maximum_vertices = 65'536U;
    constexpr std::size_t maximum_indices = 196'608U;
    if (color_interpolator >= 16U) {
        return {{}, {}, 0U, "Xenos color export must select interpolator 0..15"};
    }
    if (vertices.empty() || indices.empty() || indices.size() % 3U != 0U) {
        return {{}, {}, 0U, "Xenos triangle draw requires vertices and complete triangles"};
    }
    if (vertices.size() > maximum_vertices || indices.size() > maximum_indices) {
        return {{}, {}, 0U, "Xenos triangle draw exceeds the diagnostic buffer budget"};
    }
    for (const auto index : indices) {
        if (index >= vertices.size()) {
            return {{}, {}, 0U, "Xenos triangle references a missing vertex"};
        }
    }

    std::vector<VulkanTestVertex> backend_vertices;
    backend_vertices.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        if (vertex.position_write_mask != 0x0fU ||
            vertex.interpolator_write_masks[color_interpolator] != 0x0fU) {
            return {{}, {}, 0U,
                    "Xenos triangle vertex is missing position or selected color components"};
        }
        backend_vertices.push_back(
            {vertex.position, vertex.interpolators[color_interpolator]});
    }
    return render_offscreen_indexed_triangles(backend_vertices, indices);
}

} // namespace project360::gpu::vulkan
