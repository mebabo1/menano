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
        : info(info),
          swapchain(swapchain),
          swapchainImages(swapchainImages),
          extent(extent) {

    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& globalConf = Config::globalConf;
    auto& conf = *Config::currentConf;

    const VkFormat format =
        conf.hdr
            ? VK_FORMAT_R16G16B16A16_SFLOAT
            : VK_FORMAT_R8G8B8A8_UNORM;

    std::array<int, 2> fds{
        -1,
        -1
    };

    std::vector<int> outFds(
        conf.multiplier - 1,
        -1
    );

    /*
     * create input frames
     */
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
        &fds.at(0)
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
        &fds.at(1)
    );

    std::cerr
        << "frame fd0 = "
        << fds.at(0)
        << std::endl;

    std::cerr
        << "frame fd1 = "
        << fds.at(1)
        << std::endl;

    /*
     * create generated output images
     */
    for (size_t i = 0; i < (conf.multiplier - 1); ++i) {

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
            &outFds.at(i)
        );

        std::cerr
            << "out fd[" << i << "] = "
            << outFds.at(i)
            << std::endl;
    }

    /*
     * validate exported fds
     */
    bool fdValid =
        (fds.at(0) >= 0) &&
        (fds.at(1) >= 0);

    for (auto fd : outFds)
        fdValid &= (fd >= 0);

    if (!fdValid) {

        std::cerr
            << "lsfg-vk: external memory export failed"
            << std::endl;

        throw std::runtime_error(
            "External Vulkan memory export unavailable"
        );
    }

    auto* lsfgInitialize =
        LSFG_3_1::initialize;

    auto* lsfgCreateContext =
        LSFG_3_1::createContext;

    auto* lsfgDeleteContext =
        LSFG_3_1::deleteContext;

    if (conf.performance) {

        lsfgInitialize =
            LSFG_3_1P::initialize;

        lsfgCreateContext =
            LSFG_3_1P::createContext;

        lsfgDeleteContext =
            LSFG_3_1P::deleteContext;
    }

    setenv("DISABLE_LSFG", "1", 1);

    lsfgInitialize(
        Utils::getDeviceUUID(
            info.physicalDevice
        ),
        conf.hdr,
        1.0F / conf.flowScale,
        conf.multiplier - 1,
        globalConf.no_fp16,
        Extract::getShader
    );

    this->lsfgCtxId =
        std::shared_ptr<int32_t>(
            new int32_t(
                lsfgCreateContext(
                    fds.at(0),
                    fds.at(1),
                    outFds,
                    extent,
                    format
                )
            ),
            [lsfgDeleteContext](
                const int32_t* id
            ) {
                lsfgDeleteContext(*id);
            }
        );

    unsetenv("DISABLE_LSFG");

    this->cmdPool =
        Mini::CommandPool(
            info.device,
            info.queue.first
        );

    /*
     * allocate frame resources
     */
    for (size_t i = 0; i < 8; i++) {

        auto& pass =
            this->passInfos.at(i);

        pass.renderSemaphores.resize(
            conf.multiplier - 1
        );

        pass.acquireSemaphores.resize(
            conf.multiplier - 1
        );

        pass.postCopyBufs.resize(
            conf.multiplier - 1
        );

        pass.postCopySemaphores.resize(
            conf.multiplier - 1
        );

        pass.prevPostCopySemaphores.resize(
            conf.multiplier - 1
        );
    }
}

/*
 * IMPORTANT:
 * constructor ends ABOVE
 * present() starts BELOW
 */
VkResult LsContext::present(
    const Hooks::DeviceInfo& info,
    const void* pNext,
    VkQueue queue,
    const std::vector<VkSemaphore>& gameRenderSemaphores,
    uint32_t presentIdx)
{
    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& conf = *Config::currentConf;
    auto& pass = this->passInfos.at(this->frameIdx % 8);

    /*
     * 1. Acquire swapchain image
     */
    pass.acquireSemaphores.at(0) = Mini::Semaphore(info.device);

    uint32_t imageIdx = 0;

    auto res = Layer::ovkAcquireNextImageKHR(
        info.device,
        this->swapchain,
        UINT64_MAX,
        pass.acquireSemaphores.at(0).handle(),
        VK_NULL_HANDLE,
        &imageIdx
    );

    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw LSFG::vulkan_error(res, "Acquire failed");

    /*
     * 2. Copy swapchain → LSFG input
     */
    int preFd = -1;

    pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
    pass.preCopyBuf.begin();

    Utils::copyImage(
        pass.preCopyBuf.handle(),
        this->swapchainImages.at(imageIdx),
        (this->frameIdx % 2 == 0)
            ? this->frame_0.handle()
            : this->frame_1.handle(),
        this->extent.width,
        this->extent.height,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        true,
        false
    );

    pass.preCopyBuf.end();

    /*
     * FIX: semaphore type MUST be VkSemaphore (NOT pointers)
     */
    std::vector<VkSemaphore> preWaitSemaphores;
    std::vector<VkSemaphore> preSignalSemaphores;

    for (auto& s : gameRenderSemaphores)
        preWaitSemaphores.push_back(s);

    VkSemaphore preCopySignal =
        Mini::Semaphore(info.device).handle();

    preSignalSemaphores.push_back(preCopySignal);

    pass.preCopyBuf.submit(
        info.queue.second,
        preWaitSemaphores,
        preSignalSemaphores
    );

    /*
     * 3. LSFG execution
     */
    std::vector<int> outFds(conf.multiplier - 1);

    LSFG_3_1::presentContext(
        *this->lsfgCtxId,
        preFd,
        outFds
    );

    /*
     * 4. Copy LSFG output → swapchain
     */
    pass.postCopyBufs.at(0) =
        Mini::CommandBuffer(info.device, this->cmdPool);

    pass.postCopyBufs.at(0).begin();

    Utils::copyImage(
        pass.postCopyBufs.at(0).handle(),
        this->out_n.at(0).handle(),
        this->swapchainImages.at(imageIdx),
        this->extent.width,
        this->extent.height,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        false,
        true
    );

    pass.postCopyBufs.at(0).end();

    /*
     * FIX: correct semaphore usage
     */
    VkSemaphore postSignal =
        Mini::Semaphore(info.device).handle();

    std::vector<VkSemaphore> postWaitSemaphores = {
        preCopySignal
    };

    std::vector<VkSemaphore> postSignalSemaphores = {
        postSignal
    };

    pass.postCopyBufs.at(0).submit(
        info.queue.second,
        postWaitSemaphores,
        postSignalSemaphores
    );

    /*
     * 5. Present
     */
    VkSemaphore wait = postSignal;

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &wait;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &this->swapchain;
    presentInfo.pImageIndices = &imageIdx;
    presentInfo.pNext = pNext;

    auto result = Layer::ovkQueuePresentKHR(queue, &presentInfo);

    this->frameIdx++;

    return result;
}
