#include "utils/utils.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

#include <unordered_map>
#include <algorithm>
#include <optional>
#include <iostream>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <utility>
#include <fstream>
#include <string>
#include <vector>
#include <array>

using namespace Utils;

// =========================
// QUEUE
// =========================
std::pair<uint32_t, VkQueue> Utils::findQueue(
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    VkDeviceCreateInfo* desc,
    VkQueueFlags flags)
{
    std::vector<VkDeviceQueueCreateInfo> enabledQueues(desc->queueCreateInfoCount);
    std::copy_n(desc->pQueueCreateInfos, enabledQueues.size(), enabledQueues.data());

    uint32_t familyCount{};
    Layer::ovkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);

    std::vector<VkQueueFamilyProperties> families(familyCount);
    Layer::ovkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

    std::optional<uint32_t> idx;

    for (const auto& queueInfo : enabledQueues) {
        if ((queueInfo.queueFamilyIndex < families.size()) &&
            (families[queueInfo.queueFamilyIndex].queueFlags & flags)) {
            idx = queueInfo.queueFamilyIndex;
            break;
        }
    }

    if (!idx.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "No suitable queue found");

    VkQueue queue{};
    Layer::ovkGetDeviceQueue(device, *idx, 0, &queue);

    Layer::ovkSetDeviceLoaderData(device, queue);

    return { *idx, queue };
}

// =========================
// DEVICE UUID
// =========================
uint64_t Utils::getDeviceUUID(VkPhysicalDevice physicalDevice)
{
    VkPhysicalDeviceProperties properties{};
    Layer::ovkGetPhysicalDeviceProperties(physicalDevice, &properties);

    return (uint64_t)properties.vendorID << 32 | properties.deviceID;
}

// =========================
// SWAPCHAIN LIMIT
// =========================
uint32_t Utils::getMaxImageCount(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
    VkSurfaceCapabilitiesKHR cap{};
    auto res = Layer::ovkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice, surface, &cap);

    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed surface caps");

    return cap.maxImageCount ? cap.maxImageCount : 999;
}

// =========================
// EXTENSIONS
// =========================
std::vector<const char*> Utils::addExtensions(
    const char* const* extensions,
    size_t count,
    const std::vector<const char*>& requiredExtensions)
{
    std::vector<const char*> ext(count);
    std::copy_n(extensions, count, ext.data());

    for (auto& e : requiredExtensions) {
        auto it = std::ranges::find_if(ext,
            [e](const char* x) { return std::string(x) == e; });

        if (it == ext.end())
            ext.push_back(e);
    }

    return ext;
}

// =========================
// GPU → GPU COPY
// =========================
void Utils::copyImage(
    VkCommandBuffer buf,
    VkImage src,
    VkImage dst,
    uint32_t width,
    uint32_t height,
    VkPipelineStageFlags pre,
    VkPipelineStageFlags post,
    bool makeSrcPresentable,
    bool makeDstPresentable)
{
    VkImageMemoryBarrier barriers[2]{};

    barriers[0] = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = src,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}
    };

    barriers[1] = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = dst,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}
    };

    Layer::ovkCmdPipelineBarrier(buf,
        pre, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,nullptr,0,nullptr,2,barriers);

    VkImageBlit blit{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets = {{0,0,0},{(int32_t)width,(int32_t)height,1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstOffsets = {{0,0,0},{(int32_t)width,(int32_t)height,1}}
    };

    Layer::ovkCmdBlitImage(
        buf,
        src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,&blit,
        VK_FILTER_NEAREST
    );
}

// =========================================================
// ⭐ NEW: CPU → GPU UPLOAD
// =========================================================
void Utils::uploadBufferToImage(
    VkDevice device,
    VkCommandPool pool,
    VkQueue queue,
    const void* data,
    VkImage image,
    uint32_t width,
    uint32_t height,
    VkFormat)
{
    VkDeviceSize size = width * height * 4;

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;

    VkBufferCreateInfo bufInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    };

    vkCreateBuffer(device, &bufInfo, nullptr, &stagingBuffer);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &req);

    VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = 0 // simplified (should be proper finder)
    };

    vkAllocateMemory(device, &alloc, nullptr, &stagingMemory);
    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

    void* mapped;
    vkMapMemory(device, stagingMemory, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(device, stagingMemory);

    VkCommandBufferAllocateInfo cmdAlloc{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cmdAlloc, &cmd);

    vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    }));

    VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,nullptr,0,nullptr,1,&barrier);

    VkBufferImageCopy region{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent = {width,height,1}
    };

    vkCmdCopyBufferToImage(
        cmd,
        stagingBuffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region
    );

    vkEndCommandBuffer(cmd);

    vkQueueSubmit(queue,1,&VkSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd
    }, VK_NULL_HANDLE);

    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device,pool,1,&cmd);
    vkDestroyBuffer(device,stagingBuffer,nullptr);
    vkFreeMemory(device,stagingMemory,nullptr);
}

// =========================
// LOG
// =========================
namespace {
    auto& logCounts() {
        static std::unordered_map<std::string,size_t> m;
        return m;
    }
}

void Utils::logLimitN(const std::string& id,size_t n,const std::string& msg)
{
    auto& c = logCounts()[id];
    if (c <= n)
        std::cerr << "lsfg-vk: " << msg << "\n";
    if (c == n)
        std::cerr << "(suppressed)\n";
    c++;
}

void Utils::resetLimitN(const std::string& id) noexcept
{
    logCounts().erase(id);
}

// =========================
// PROCESS
// =========================
std::pair<std::string,std::string> Utils::getProcessName()
{
    return { "proc", "proc" };
}

// =========================
// CONFIG
// =========================
std::string Utils::getConfigFile()
{
    return "/etc/lsfg-vk/conf.toml";
}
