#include "context.hpp"
#include "config/config.hpp"
#include "common/exception.hpp"
#include "extract/extract.hpp"
#include "utils/utils.hpp"
#include "hooks.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>
#include <lsfg_3_1.hpp>
#include <lsfg_3_1p.hpp>

#include <stdexcept>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <memory>
#include <string>
#include <array>
#include <cstring>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// =======================================================
// LOCAL HELPER (Utils 대체)
// =======================================================

static void uploadBufferToImage(
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    VkQueue queue,
    VkCommandPool cmdPool,
    const void* data,
    VkImage image,
    uint32_t width,
    uint32_t height
) {
    VkDeviceSize size = width * height * 4;

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;

    VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer);

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReq.size,
        .memoryTypeIndex = 0
    };

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }

    vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

    void* mapped;
    vkMapMemory(device, stagingMemory, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(device, stagingMemory);

    VkCommandBufferAllocateInfo allocCmd{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocCmd, &cmd);

    VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier toTransfer{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
    };

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &toTransfer
    );

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(
        cmd,
        stagingBuffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region
    );

    VkImageMemoryBarrier toShader{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
    };

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &toShader
    );

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd
    };

    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
}

// =======================================================
// CONSTRUCTOR
// =======================================================

LsContext::LsContext(
    const Hooks::DeviceInfo& info,
    VkSwapchainKHR swapchain,
    VkExtent2D extent,
    const std::vector<VkImage>& swapchainImages
)
: swapchain(swapchain),
  swapchainImages(swapchainImages),
  extent(extent),
  device(info.device),
  physicalDevice(info.physicalDevice),
  queue(info.queue.second)
{
    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& globalConf = Config::globalConf;
    auto& conf = *Config::currentConf;

    VkFormat format = conf.hdr
        ? VK_FORMAT_R8G8B8A8_UNORM
        : VK_FORMAT_R16G16B16A16_SFLOAT;

    std::array<int, 2> fds{};

    frame_0 = Mini::Image(info.device, info.physicalDevice, extent, format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds[0]);

    frame_1 = Mini::Image(info.device, info.physicalDevice, extent, format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds[1]);

    std::vector<int> outFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; i++) {
        out_n.emplace_back(info.device, info.physicalDevice, extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &outFds[i]);
    }

    auto* init    = conf.performance ? LSFG_3_1P::initialize : LSFG_3_1::initialize;
    auto* create  = conf.performance ? LSFG_3_1P::createContext : LSFG_3_1::createContext;
    auto* destroy = conf.performance ? LSFG_3_1P::deleteContext : LSFG_3_1::deleteContext;

    setenv("DISABLE_LSFG", "1", 1);

    init(
        Utils::getDeviceUUID(info.physicalDevice),
        conf.hdr,
        1.0F / conf.flowScale,
        conf.multiplier - 1,
        globalConf.no_fp16,
        Extract::getShader
    );

    ipcMode = IPCMode::FD;
    fdFailed = false;

    int ctx = create(fds[0], fds[1], outFds, extent, format);

    if (ctx < 0) {
        ipcMode = IPCMode::SHM;
        fdFailed = true;

        shmFrameSize = extent.width * extent.height * 4;

        std::string base = "/data/data/com.termux/files/usr/tmp/lsfg_";

        for (int i = 0; i < conf.multiplier - 1; i++) {
            std::string inName  = base + "in_"  + std::to_string(i);
            std::string outName = base + "out_" + std::to_string(i);

            int shmIn  = shm_open(inName.c_str(), O_CREAT | O_RDWR, 0666);
            int shmOut = shm_open(outName.c_str(), O_CREAT | O_RDWR, 0666);

            ftruncate(shmIn, shmFrameSize);
            ftruncate(shmOut, shmFrameSize);

            shmInputs.push_back(mmap(nullptr, shmFrameSize,
                PROT_READ | PROT_WRITE, MAP_SHARED, shmIn, 0));

            shmOutputs.push_back(mmap(nullptr, shmFrameSize,
                PROT_READ | PROT_WRITE, MAP_SHARED, shmOut, 0));
        }

        ctx = create(0, 0, {}, extent, format);
    }

    lsfgCtxId = std::shared_ptr<int32_t>(
        new int32_t(ctx),
        [destroy](const int32_t* id) {
            destroy(*id);
        }
    );

    unsetenv("DISABLE_LSFG");

    cmdPool = Mini::CommandPool(info.device, info.queue.first);
}

// =======================================================
// PRESENT
// =======================================================

VkResult LsContext::present(
    const Hooks::DeviceInfo& info,
    const void* pNext,
    VkQueue queue,
    const std::vector<VkSemaphore>& gameRenderSemaphores,
    uint32_t presentIdx
)
{
    auto& conf = *Config::currentConf;
    auto& pass = passInfos[frameIdx % 8];

    int preCopySemaphoreFd{};

    pass.preCopySemaphores[0] =
        Mini::Semaphore(info.device, &preCopySemaphoreFd);

    pass.preCopyBuf = Mini::CommandBuffer(info.device, cmdPool);
    pass.preCopyBuf.begin();

    Utils::copyImage(
        pass.preCopyBuf.handle(),
        swapchainImages[presentIdx],
        frameIdx % 2 == 0 ? frame_0.handle() : frame_1.handle(),
        extent.width,
        extent.height,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        true,
        false
    );

    pass.preCopyBuf.end();

    pass.preCopyBuf.submit(
        info.queue.second,
        gameRenderSemaphores,
        { pass.preCopySemaphores[0].handle() }
    );

    std::vector<int> renderFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; i++) {
        pass.renderSemaphores[i] =
            Mini::Semaphore(info.device, &renderFds[i]);
    }

    bool useFdPath = (!fdFailed && ipcMode == IPCMode::FD && lsfgCtxId);

    if (useFdPath) {
        LSFG_3_1::presentContext(*lsfgCtxId, preCopySemaphoreFd, renderFds);
    } else {
        std::vector<uint8_t> cpu(shmFrameSize);
        memcpy(cpu.data(), shmOutputs[0], shmFrameSize);

        uploadBufferToImage(
            info.device,
            info.physicalDevice,
            info.queue.second,
            cmdPool.handle(),
            cpu.data(),
            frameIdx % 2 == 0 ? frame_0.handle() : frame_1.handle(),
            extent.width,
            extent.height
        );

        LSFG_3_1::presentContext(*lsfgCtxId, preCopySemaphoreFd, renderFds);
    }

    frameIdx++;
    return VK_SUCCESS;
}
