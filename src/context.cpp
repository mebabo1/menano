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

    VkFormat format = conf.hdr
        ? VK_FORMAT_R8G8B8A8_UNORM
        : VK_FORMAT_R16G16B16A16_SFLOAT;

    /* =========================
     * FRAME STORAGE (NO FD DEPENDENCY)
     * ========================= */
    std::array<int, 2> fds{ -1, -1 };

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
     * OUTPUT FRAMES
     * ========================= */
    std::vector<int> outFds(conf.multiplier - 1, -1);

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
     * LSFG INIT (NO EXTERNAL SYNC ASSUMPTION)
     * ========================= */
    auto* init = conf.performance
        ? LSFG_3_1P::initialize
        : LSFG_3_1::initialize;

    auto* createCtx = conf.performance
        ? LSFG_3_1P::createContext
        : LSFG_3_1::createContext;

    auto* destroyCtx = conf.performance
        ? LSFG_3_1P::deleteContext
        : LSFG_3_1::deleteContext;

    init(
        Utils::getDeviceUUID(info.physicalDevice),
        conf.hdr,
        1.0F / conf.flowScale,
        conf.multiplier - 1,
        globalConf.no_fp16,
        Extract::getShader
    );

    /* =========================
     * CONTEXT CREATION (INDEPENDENT OF FD)
     * ========================= */
    lsfgCtxId = std::shared_ptr<int32_t>(
        new int32_t(
            createCtx(
                fds.at(0),
                fds.at(1),
                outFds,
                extent,
                format
            )
        ),
        [destroyCtx](const int32_t* id) {
            destroyCtx(*id);
        }
    );

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
     * LSFG EXECUTION (NO MODE COUPLING TO FD)
     * ========================= */

    std::vector<int> renderFds(conf.multiplier - 1, -1);

    for (size_t i = 0; i < conf.multiplier - 1; ++i) {
        pass.renderSemaphores.at(i) =
            Mini::Semaphore(info.device, &renderFds.at(i));
    }

    /*
     * 핵심 변경:
     * - fd는 LSFG mode 결정에 사용하지 않음
     * - LSFG는 항상 동일 context 경로 사용
     */
    LSFG_3_1::presentContext(
        *lsfgCtxId,
        preCopyFd,     // optional only
        renderFds
    );

    /* =========================
     * PRESENT LOOP
     * ========================= */
    for (size_t i = 0; i < conf.multiplier - 1; i++) {

        pass.acquireSemaphores.at(i) =
            Mini::Semaphore(info.device);

        uint32_t imageIdx{};

        Layer::ovkAcquireNextImageKHR(
            info.device,
            swapchain,
            UINT64_MAX,
            pass.acquireSemaphores.at(i).handle(),
            VK_NULL_HANDLE,
            &imageIdx
        );

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

        Layer::ovkQueuePresentKHR(queue, &presentInfo);
    }

    frameIdx++;
    return VK_SUCCESS;
}
