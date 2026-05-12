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

LsContext::LsContext(
    const Hooks::DeviceInfo& info,
    VkSwapchainKHR swapchain,
    VkExtent2D extent,
    const std::vector<VkImage>& swapchainImages)
    : swapchain(swapchain),
      swapchainImages(swapchainImages),
      extent(extent)
{
    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& globalConf = Config::globalConf;
    auto& conf = *Config::currentConf;

    const VkFormat format = conf.hdr
        ? VK_FORMAT_R8G8B8A8_UNORM
        : VK_FORMAT_R16G16B16A16_SFLOAT;

    /* -----------------------------------------------------
     * Frame resources (double buffer)
     * ----------------------------------------------------- */
    std::array<int, 2> fds{};

    frame_0 = Mini::Image(
        info.device, info.physicalDevice,
        extent, format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds[0]);

    frame_1 = Mini::Image(
        info.device, info.physicalDevice,
        extent, format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds[1]);

    std::vector<int> outFds(conf.multiplier - 1);

    for (size_t i = 0; i < (conf.multiplier - 1); ++i) {
        out_n.emplace_back(
            info.device, info.physicalDevice,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &outFds[i]);
    }

    /* -----------------------------------------------------
     * LSFG init
     * ----------------------------------------------------- */
    auto* lsfgInitialize = LSFG_3_1::initialize;
    auto* lsfgCreateContext = LSFG_3_1::createContext;
    auto* lsfgDeleteContext = LSFG_3_1::deleteContext;

    if (conf.performance) {
        lsfgInitialize = LSFG_3_1P::initialize;
        lsfgCreateContext = LSFG_3_1P::createContext;
        lsfgDeleteContext = LSFG_3_1P::deleteContext;
    }

    setenv("DISABLE_LSFG", "1", 1);

    lsfgInitialize(
        Utils::getDeviceUUID(info.physicalDevice),
        conf.hdr,
        1.0F / conf.flowScale,
        conf.multiplier - 1,
        globalConf.no_fp16,
        Extract::getShader
    );

    lsfgCtxId = std::shared_ptr<int32_t>(
        new int32_t(lsfgCreateContext(
            fds[0],
            fds[1],
            outFds,
            extent,
            format)),
        [lsfgDeleteContext](const int32_t* id) {
            lsfgDeleteContext(*id);
        }
    );

    unsetenv("DISABLE_LSFG");

    /* -----------------------------------------------------
     * Command pool
     * ----------------------------------------------------- */
    cmdPool = Mini::CommandPool(info.device, info.queue.first);

    for (auto& pass : passInfos) {
        pass.renderSemaphores.resize(conf.multiplier - 1);
        pass.acquireSemaphores.resize(conf.multiplier - 1);
        pass.postCopyBufs.resize(conf.multiplier - 1);
        pass.postCopySemaphores.resize(conf.multiplier - 1);
        pass.prevPostCopySemaphores.resize(conf.multiplier - 1);
    }
}

/* =========================================================
 * PRESENT (FIXED PIPELINE)
 * ========================================================= */
VkResult LsContext::present(
    const Hooks::DeviceInfo& info,
    const void* pNext,
    VkQueue queue,
    const std::vector<VkSemaphore>& gameRenderSemaphores,
    uint32_t presentIdx)
{
    auto& conf = *Config::currentConf;
    auto& pass = passInfos[frameIdx % 8];

    /* -----------------------------------------------------
     * 1. COPY SWAPCHAIN → FRAME BUFFER
     * ----------------------------------------------------- */
    pass.preCopyBuf = Mini::CommandBuffer(info.device, cmdPool);
    pass.preCopyBuf.begin();

    Utils::copyImage(
        pass.preCopyBuf.handle(),
        swapchainImages[presentIdx],
        (frameIdx % 2 == 0) ? frame_0.handle() : frame_1.handle(),
        extent.width,
        extent.height,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        true,
        false
    );

    pass.preCopyBuf.end();

    Mini::Semaphore preCopySem(info.device);

    pass.preCopyBuf.submit(
        info.queue.second,
        gameRenderSemaphores,
        { preCopySem.handle() }
    );

    /* -----------------------------------------------------
     * 2. LSFG GENERATION
     * ----------------------------------------------------- */
    std::vector<int> renderFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; i++) {
        pass.renderSemaphores[i] =
            Mini::Semaphore(info.device, &renderFds[i]);
    }

    if (conf.performance) {
        LSFG_3_1P::presentContext(*lsfgCtxId, preCopySemFd, renderFds);
    } else {
        LSFG_3_1::presentContext(*lsfgCtxId, preCopySemFd, renderFds);
    }

    /* -----------------------------------------------------
     * 3. OUTPUT → SWAPCHAIN (FIXED LINEAR PRESENT CHAIN)
     * ----------------------------------------------------- */
    VkSemaphore lastSignal = preCopySem.handle();

    for (size_t i = 0; i < conf.multiplier - 1; i++) {

        uint32_t imageIdx = 0;

        vkAcquireNextImageKHR(
            info.device,
            swapchain,
            UINT64_MAX,
            lastSignal,
            VK_NULL_HANDLE,
            &imageIdx
        );

        pass.postCopyBufs[i] = Mini::CommandBuffer(info.device, cmdPool);
        pass.postCopyBufs[i].begin();

        Utils::copyImage(
            pass.postCopyBufs[i].handle(),
            out_n[i].handle(),
            swapchainImages[imageIdx],
            extent.width,
            extent.height,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            false,
            true
        );

        pass.postCopyBufs[i].end();

        Mini::Semaphore presentSem(info.device);

        pass.postCopyBufs[i].submit(
            info.queue.second,
            { pass.renderSemaphores[i].handle() },
            { presentSem.handle() }
        );

        VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &presentSem.handle(),
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIdx
        };

        Layer::ovkQueuePresentKHR(queue, &presentInfo);

        lastSignal = presentSem.handle();
    }

    frameIdx++;
    return VK_SUCCESS;
}
