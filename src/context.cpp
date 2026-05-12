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
#include <iostream>

LsContext::LsContext(
    const Hooks::DeviceInfo& info,
    VkSwapchainKHR swapchain,
    VkExtent2D extent,
    const std::vector<VkImage>& swapchainImages)
    : swapchain(swapchain),
      swapchainImages(swapchainImages),
      extent(extent),
      lsfgEnabled(true)
{
    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& globalConf = Config::globalConf;
    auto& conf = *Config::currentConf;

    const VkFormat format = conf.hdr
        ? VK_FORMAT_R8G8B8A8_UNORM
        : VK_FORMAT_R16G16B16A16_SFLOAT;

    /* =========================
     * INPUT IMAGES
     * ========================= */
    std::array<int, 2> fds{};

    frame_0 = Mini::Image(
        info.device, info.physicalDevice,
        extent, format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds.at(0)
    );

    frame_1 = Mini::Image(
        info.device, info.physicalDevice,
        extent, format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds.at(1)
    );

    /* =========================
     * OUTPUT IMAGES
     * ========================= */
    std::vector<int> outFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; ++i) {
        out_n.emplace_back(
            info.device, info.physicalDevice,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &outFds.at(i)
        );
    }

    /* =========================
     * LSFG INIT
     * ========================= */
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
        new int32_t(
            lsfgCreateContext(
                fds.at(0),
                fds.at(1),
                outFds,
                extent,
                format
            )
        ),
        [lsfgDeleteContext](const int32_t* id) {
            lsfgDeleteContext(*id);
        }
    );

    unsetenv("DISABLE_LSFG");

    /* =========================
     * COMMAND POOL
     * ========================= */
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
 * PRESENT
 * ========================================================= */
VkResult LsContext::present(
    const Hooks::DeviceInfo& info,
    const void* pNext,
    VkQueue queue,
    const std::vector<VkSemaphore>& gameRenderSemaphores,
    uint32_t presentIdx)
{
    auto& conf = *Config::currentConf;
    auto& pass = passInfos.at(frameIdx % 8);

    /* =========================
     * PRE COPY
     * ========================= */
    int preCopyFd = -1;

    pass.preCopySemaphores.at(0) =
        Mini::Semaphore(info.device, &preCopyFd);

    pass.preCopySemaphores.at(1) =
        Mini::Semaphore(info.device);

    pass.preCopyBuf = Mini::CommandBuffer(info.device, cmdPool);
    pass.preCopyBuf.begin();

    Utils::copyImage(
        pass.preCopyBuf.handle(),
        swapchainImages.at(presentIdx),
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
        {
            pass.preCopySemaphores.at(0).handle(),
            pass.preCopySemaphores.at(1).handle()
        }
    );

    /* =========================
     * LSFG ENABLE CHECK
     * ========================= */
    if (preCopyFd < 0) {
        lsfgEnabled = false;
    }

    /* =====================================================
     * ⭐ FALLBACK PATH (핵심 수정)
     * ===================================================== */
    if (!lsfgEnabled) {

        std::cerr << "lsfg-vk: fallback active (direct present)" << std::endl;

        VkPresentInfoKHR fallbackPresent{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = pNext,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &pass.preCopySemaphores.at(1).handle(),
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &presentIdx
        };

        auto res = Layer::ovkQueuePresentKHR(queue, &fallbackPresent);

        frameIdx++;
        return res;
    }

    /* =========================
     * LSFG PATH
     * ========================= */
    std::vector<int> renderFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; ++i) {
        pass.renderSemaphores.at(i) =
            Mini::Semaphore(info.device, &renderFds.at(i));
    }

    if (conf.performance) {
        LSFG_3_1P::presentContext(*lsfgCtxId, preCopyFd, renderFds);
    } else {
        LSFG_3_1::presentContext(*lsfgCtxId, preCopyFd, renderFds);
    }

    /* =========================
     * PRESENT GENERATED FRAMES
     * ========================= */
    for (size_t i = 0; i < conf.multiplier - 1; i++) {

        pass.acquireSemaphores.at(i) =
            Mini::Semaphore(info.device);

        uint32_t imageIdx{};

        auto res = Layer::ovkAcquireNextImageKHR(
            info.device,
            swapchain,
            UINT64_MAX,
            pass.acquireSemaphores.at(i).handle(),
            VK_NULL_HANDLE,
            &imageIdx
        );

        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw LSFG::vulkan_error(res, "Acquire failed");

        pass.postCopySemaphores.at(i) =
            Mini::Semaphore(info.device);

        pass.prevPostCopySemaphores.at(i) =
            Mini::Semaphore(info.device);

        pass.postCopyBufs.at(i) =
            Mini::CommandBuffer(info.device, cmdPool);

        pass.postCopyBufs.at(i).begin();

        Utils::copyImage(
            pass.postCopyBufs.at(i).handle(),
            out_n.at(i).handle(),
            swapchainImages.at(imageIdx),
            extent.width,
            extent.height,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            false,
            true
        );

        pass.postCopyBufs.at(i).end();

        pass.postCopyBufs.at(i).submit(
            info.queue.second,
            {
                pass.acquireSemaphores.at(i).handle(),
                pass.renderSemaphores.at(i).handle()
            },
            {
                pass.postCopySemaphores.at(i).handle(),
                pass.prevPostCopySemaphores.at(i).handle()
            }
        );

        std::vector<VkSemaphore> waits{
            pass.postCopySemaphores.at(i).handle()
        };

        if (i != 0)
            waits.emplace_back(
                pass.prevPostCopySemaphores.at(i - 1).handle()
            );

        VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = i == 0 ? pNext : nullptr,
            .waitSemaphoreCount = (uint32_t)waits.size(),
            .pWaitSemaphores = waits.data(),
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIdx
        };

        res = Layer::ovkQueuePresentKHR(queue, &presentInfo);

        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw LSFG::vulkan_error(res, "Present failed");
    }

    frameIdx++;
    return VK_SUCCESS;
}
