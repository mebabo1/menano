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

enum class LsfgMode {
    ExternalFd,
    InternalVk
};

LsContext::LsContext(
    const Hooks::DeviceInfo& info,
    VkSwapchainKHR swapchain,
    VkExtent2D extent,
    const std::vector<VkImage>& swapchainImages)
    : info(info),
      swapchain(swapchain),
      swapchainImages(swapchainImages),
      extent(extent) {

    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& globalConf = Config::globalConf;
    auto& conf = *Config::currentConf;

    VkFormat format =
        conf.hdr
            ? VK_FORMAT_R16G16B16A16_SFLOAT
            : VK_FORMAT_R8G8B8A8_UNORM;

    std::array<int, 2> fds{-1, -1};
    std::vector<int> outFds(conf.multiplier - 1, -1);

    bool fdSupported = true;

    this->frame_0 = Mini::Image(
        info.device,
        info.physicalDevice,
        extent,
        format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        fdSupported ? &fds[0] : nullptr
    );

    this->frame_1 = Mini::Image(
        info.device,
        info.physicalDevice,
        extent,
        format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        fdSupported ? &fds[1] : nullptr
    );

    for (size_t i = 0; i < conf.multiplier - 1; i++) {
        this->out_n.emplace_back(
            info.device,
            info.physicalDevice,
            extent,
            format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            fdSupported ? &outFds[i] : nullptr
        );
    }

    bool fdValid =
        (fds[0] >= 0 && fds[1] >= 0);

    for (auto fd : outFds)
        fdValid &= (fd >= 0);

    this->mode = fdValid
        ? LsfgMode::ExternalFd
        : LsfgMode::InternalVk;

    auto* init = LSFG_3_1::initialize;
    auto* create = LSFG_3_1::createContext;
    auto* destroy = LSFG_3_1::deleteContext;

    if (conf.performance) {
        init = LSFG_3_1P::initialize;
        create = LSFG_3_1P::createContext;
        destroy = LSFG_3_1P::deleteContext;
    }

    setenv("DISABLE_LSFG", "1", 1);

    init(
        Utils::getDeviceUUID(info.physicalDevice),
        conf.hdr,
        1.0F / conf.flowScale,
        conf.multiplier - 1,
        globalConf.no_fp16,
        Extract::getShader
    );

    if (mode == LsfgMode::ExternalFd) {

        this->lsfgCtxId =
            std::shared_ptr<int32_t>(
                new int32_t(
                    create(
                        fds[0],
                        fds[1],
                        outFds,
                        extent,
                        format
                    )
                ),
                [destroy](const int32_t* id) {
                    destroy(*id);
                }
            );

    } else {

        this->lsfgCtxId =
            std::shared_ptr<int32_t>(
                new int32_t(
                    LSFG_3_1::createContextVk(
                        frame_0.handle(),
                        frame_1.handle(),
                        out_n_handles,
                        extent,
                        format
                    )
                ),
                [destroy](const int32_t* id) {
                    destroy(*id);
                }
            );
    }

    unsetenv("DISABLE_LSFG");

    this->cmdPool =
        Mini::CommandPool(info.device, info.queue.first);
}

VkResult LsContext::present(
    const Hooks::DeviceInfo& info,
    const void* pNext,
    VkQueue queue,
    const std::vector<VkSemaphore>& gameRenderSemaphores,
    uint32_t presentIdx) {

    auto& conf = *Config::currentConf;
    auto& pass = this->passInfos[this->frameIdx % 8];

    int preCopyFd{};

    pass.preCopySemaphores[0] =
        Mini::Semaphore(info.device, &preCopyFd);

    pass.preCopySemaphores[1] =
        Mini::Semaphore(info.device);

    pass.preCopyBuf =
        Mini::CommandBuffer(info.device, this->cmdPool);

    pass.preCopyBuf.begin();

    Utils::copyImage(
        pass.preCopyBuf.handle(),
        this->swapchainImages[presentIdx],
        (frameIdx % 2 == 0) ? frame_0.handle() : frame_1.handle(),
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
            pass.preCopySemaphores[0].handle(),
            pass.preCopySemaphores[1].handle()
        }
    );

    std::vector<int> renderFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; i++) {
        pass.renderSemaphores[i] =
            Mini::Semaphore(info.device, &renderFds[i]);
    }

    if (mode == LsfgMode::ExternalFd) {

        LSFG_3_1::presentContext(
            *lsfgCtxId,
            preCopyFd,
            renderFds
        );

        for (size_t i = 0; i < conf.multiplier - 1; i++) {

            pass.acquireSemaphores[i] =
                Mini::Semaphore(info.device);

            uint32_t imageIdx{};

            Layer::ovkAcquireNextImageKHR(
                info.device,
                swapchain,
                UINT64_MAX,
                pass.acquireSemaphores[i].handle(),
                VK_NULL_HANDLE,
                &imageIdx
            );

            Utils::copyImage(
                pass.postCopyBufs[i].handle(),
                out_n[i].handle(),
                swapchainImages[imageIdx],
                extent.width,
                extent.height,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                false,
                true
            );

            Layer::ovkQueuePresentKHR(queue, nullptr);
        }

    } else {

        LSFG_3_1::presentContextVk(
            *lsfgCtxId,
            pass.preCopySemaphores[0].handle(),
            pass.renderSemaphores,
            frame_0.handle(),
            frame_1.handle(),
            out_n
        );

        uint32_t imageIdx{};

        Layer::ovkAcquireNextImageKHR(
            info.device,
            swapchain,
            UINT64_MAX,
            pass.preCopySemaphores[1].handle(),
            VK_NULL_HANDLE,
            &imageIdx
        );

        Utils::copyImage(
            pass.postCopyBufs[0].handle(),
            out_n[0].handle(),
            swapchainImages[imageIdx],
            extent.width,
            extent.height,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            false,
            true
        );

        Layer::ovkQueuePresentKHR(queue, nullptr);
    }

    this->frameIdx++;
    return VK_SUCCESS;
}
