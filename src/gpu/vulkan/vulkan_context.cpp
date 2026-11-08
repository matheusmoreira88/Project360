#include "gpu/vulkan/vulkan_context.hpp"

#include <volk.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
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
    VkDeviceMemory image_memory{VK_NULL_HANDLE};
    VkBuffer staging_buffer{VK_NULL_HANDLE};
    VkDeviceMemory staging_memory{VK_NULL_HANDLE};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    bool submission_pending{};

    ~ClearResources() {
        if (submission_pending && queue != VK_NULL_HANDLE) {
            static_cast<void>(vkQueueWaitIdle(queue));
        }
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, fence, nullptr);
        }
        if (command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, command_pool, nullptr);
        }
        if (staging_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, staging_buffer, nullptr);
        }
        if (staging_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, staging_memory, nullptr);
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
        }
        if (image_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, image_memory, nullptr);
        }
    }
};

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

} // namespace project360::gpu::vulkan
