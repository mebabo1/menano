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
#include <array>
#include <iostream>

class LsContext {
public:
    LsContext(
        const Hooks::DeviceInfo& info,
        VkSwapchainKHR swapchain,
        VkExtent2D extent,
        const std::vector<VkImage>& swapchainImages);

    VkResult present(
        const Hooks::DeviceInfo& info,
        const void* pNext,
        VkQueue queue,
        const std::vector<VkSemaphore>& gameRenderSemaphores,
        uint32_t presentIdx);

private:
    struct FrameSync {
        Mini::Semaphore imageAvailable;
        Mini::Semaphore renderFinished;
        Mini::Semaphore postCopy;
    };

    static constexpr int MAX_FRAMES = 8;
    std::array<FrameSync, MAX_FRAMES> frameSync;

    uint32_t frameIdx = 0;

    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    std::vector<VkImage> swapchainImages;

    Mini::CommandPool cmdPool;
    Hooks::DeviceInfo info;

    std::shared_ptr<int32_t> lsfgCtxId;

    Mini::Image frame_0;
    Mini::Image frame_1;
    std::vector<Mini::Image> out_n;
};

LsContext::LsContext(
    const Hooks::DeviceInfo& info,
    VkSwapchainKHR swapchain,
    VkExtent2D extent,
    const std::vector<VkImage>& swapchainImages)
    : info(info),
      swapchain(swapchain),
      swapchainImages(swapchainImages),
      extent(extent),
      cmdPool(info.device, info.queue.first)
{
    auto& conf = *Config::currentConf;

    const VkFormat format =
        conf.hdr ? VK_FORMAT_R16G16B16A16_SFLOAT
                 : VK_FORMAT_R8G8B8A8_UNORM;

    int fd0 = -1, fd1 = -1;

    frame_0 = Mini::Image(
        info.device,
        info.physicalDevice,
        extent,
        format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fd0
    );

    frame_1 = Mini::Image(
        info.device,
        info.physicalDevice,
        extent,
        format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fd1
    );

    std::vector<int> outFds(conf.multiplier - 1);

    for (int i = 0; i < conf.multiplier - 1; i++) {
        out_n.emplace_back(
            info.device,
            info.physicalDevice,
            extent,
            format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &outFds[i]
        );
    }

    bool ok = fd0 >= 0 && fd1 >= 0;
    for (auto fd : outFds) ok &= (fd >= 0);

    if (!ok)
        throw std::runtime_error("external memory export failed");

    setenv("DISABLE_LSFG", "1", 1);

    auto* init = conf.performance
        ? LSFG_3_1P::initialize
        : LSFG_3_1::initialize;

    auto* create = conf.performance
        ? LSFG_3_1P::createContext
        : LSFG_3_1::createContext;

    auto* destroy = conf.performance
        ? LSFG_3_1P::deleteContext
        : LSFG_3_1::deleteContext;

    init(
        Utils::getDeviceUUID(info.physicalDevice),
        conf.hdr,
        1.0F / conf.flowScale,
        conf.multiplier - 1,
        Config::globalConf.no_fp16,
        Extract::getShader
    );

    lsfgCtxId = std::shared_ptr<int32_t>(
        new int32_t(create(fd0, fd1, outFds, extent, format)),
        [destroy](int32_t* id) { destroy(*id); }
    );

    unsetenv("DISABLE_LSFG");
}

VkResult LsContext::present(
    const Hooks::DeviceInfo& info,
    const void* pNext,
    VkQueue queue,
    const std::vector<VkSemaphore>& gameRenderSemaphores,
    uint32_t presentIdx)
{
    auto& conf = *Config::currentConf;
    FrameSync& sync = frameSync[frameIdx % MAX_FRAMES];

    uint32_t imageIndex = 0;

    auto res = Layer::ovkAcquireNextImageKHR(
        info.device,
        swapchain,
        UINT64_MAX,
        sync.imageAvailable.handle(),
        VK_NULL_HANDLE,
        &imageIndex
    );

    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw LSFG::vulkan_error(res, "Acquire failed");

    Mini::CommandBuffer cmd(info.device, cmdPool);
    cmd.begin();

    Utils::copyImage(
        cmd.handle(),
        swapchainImages[imageIndex],
        (frameIdx % 2 == 0) ? frame_0.handle() : frame_1.handle(),
        extent.width,
        extent.height,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        true,
        false
    );

    cmd.end();

    cmd.submit(
        info.queue.second,
        gameRenderSemaphores,
        { sync.renderFinished.handle() }
    );

    std::vector<int> renderFds(conf.multiplier - 1);

    if (conf.performance)
        LSFG_3_1P::presentContext(*lsfgCtxId, -1, renderFds);
    else
        LSFG_3_1::presentContext(*lsfgCtxId, -1, renderFds);

    Mini::CommandBuffer post(info.device, cmdPool);
    post.begin();

    Utils::copyImage(
        post.handle(),
        out_n[0].handle(),
        swapchainImages[imageIndex],
        extent.width,
        extent.height,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        false,
        true
    );

    post.end();

    post.submit(
        info.queue.second,
        { sync.renderFinished.handle() },
        { sync.postCopy.handle() }
    );

    VkSemaphore wait = sync.postCopy.handle();

    VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &wait,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIndex
    };

    res = Layer::ovkQueuePresentKHR(queue, &presentInfo);

    frameIdx++;
    return res;
}
