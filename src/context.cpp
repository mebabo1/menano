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

    /*
     * correct format selection
     */
    const VkFormat format =
        conf.hdr
            ? VK_FORMAT_R16G16B16A16_SFLOAT
            : VK_FORMAT_R8G8B8A8_UNORM;

    /*
     * external memory fds
     * these are exported by Mini::Image internally
     */
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

    std::cerr
        << "createContext fd0="
        << fds.at(0)
        << " fd1="
        << fds.at(1)
        << std::endl;

    for (auto fd : outFds) {

        std::cerr
            << "createContext outfd="
            << fd
            << std::endl;
    }

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

VkResult LsContext::present(
        const Hooks::DeviceInfo& info,
        const void* pNext,
        VkQueue queue,
        const std::vector<VkSemaphore>& gameRenderSemaphores,
        uint32_t presentIdx) {

    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& conf = *Config::currentConf;

    auto& pass =
        this->passInfos.at(
            this->frameIdx % 8
        );

    int preCopySemaphoreFd{};

    /*
     * semaphore for lsfg
     */
    pass.preCopySemaphores.at(0) =
        Mini::Semaphore(
            info.device,
            &preCopySemaphoreFd
        );

    /*
     * internal sync semaphore
     */
    pass.preCopySemaphores.at(1) =
        Mini::Semaphore(
            info.device
        );

    /*
     * copy swapchain -> frame_X
     */
    pass.preCopyBuf =
        Mini::CommandBuffer(
            info.device,
            this->cmdPool
        );

    pass.preCopyBuf.begin();

    Utils::copyImage(
        pass.preCopyBuf.handle(),
        this->swapchainImages.at(
            presentIdx
        ),
        this->frameIdx % 2 == 0
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

    std::vector<VkSemaphore> preWaits =
        gameRenderSemaphores;

    /*
     * previous frame ordering
     */
    if (this->frameIdx > 0) {

        preWaits.emplace_back(
            this->passInfos.at(
                (this->frameIdx - 1) % 8
            ).preCopySemaphores.at(1)
             .handle()
        );
    }

    pass.preCopyBuf.submit(
        info.queue.second,
        preWaits,
        {
            pass.preCopySemaphores.at(0).handle(),
            pass.preCopySemaphores.at(1).handle()
        }
    );

    /*
     * semaphores signaled by lsfg
     */
    std::vector<int> renderSemaphoreFds(
        conf.multiplier - 1
    );

    for (size_t i = 0;
         i < (conf.multiplier - 1);
         ++i) {

        pass.renderSemaphores.at(i) =
            Mini::Semaphore(
                info.device,
                &renderSemaphoreFds.at(i)
            );
    }

    /*
     * run lsfg
     */
    if (conf.performance) {

        LSFG_3_1P::presentContext(
            *this->lsfgCtxId,
            preCopySemaphoreFd,
            renderSemaphoreFds
        );

    } else {

        LSFG_3_1::presentContext(
            *this->lsfgCtxId,
            preCopySemaphoreFd,
            renderSemaphoreFds
        );
    }

    /*
     * generated frame presents
     */
    for (size_t i = 0;
         i < (conf.multiplier - 1);
         i++) {

        pass.acquireSemaphores.at(i) =
            Mini::Semaphore(
                info.device
            );

        uint32_t imageIdx{};

        auto res =
            Layer::ovkAcquireNextImageKHR(
                info.device,
                this->swapchain,
                UINT64_MAX,
                pass.acquireSemaphores
                    .at(i)
                    .handle(),
                VK_NULL_HANDLE,
                &imageIdx
            );

        if (res != VK_SUCCESS &&
            res != VK_SUBOPTIMAL_KHR) {

            throw LSFG::vulkan_error(
                res,
                "Acquire failed"
            );
        }

        pass.postCopySemaphores.at(i) =
            Mini::Semaphore(
                info.device
            );

        pass.prevPostCopySemaphores.at(i) =
            Mini::Semaphore(
                info.device
            );

        pass.postCopyBufs.at(i) =
            Mini::CommandBuffer(
                info.device,
                this->cmdPool
            );

        pass.postCopyBufs.at(i).begin();

        /*
         * copy generated frame
         * out_n -> swapchain
         */
        Utils::copyImage(
            pass.postCopyBufs.at(i)
                .handle(),
            this->out_n.at(i)
                .handle(),
            this->swapchainImages.at(
                imageIdx
            ),
            this->extent.width,
            this->extent.height,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            false,
            true
        );

        pass.postCopyBufs.at(i).end();

        pass.postCopyBufs.at(i).submit(
            info.queue.second,
            {
                pass.acquireSemaphores
                    .at(i)
                    .handle(),

                pass.renderSemaphores
                    .at(i)
                    .handle()
            },
            {
                pass.postCopySemaphores
                    .at(i)
                    .handle(),

                pass.prevPostCopySemaphores
                    .at(i)
                    .handle()
            }
        );

        std::vector<VkSemaphore>
            waitSemaphores{

            pass.postCopySemaphores
                .at(i)
                .handle()
        };

        if (i != 0) {

            waitSemaphores.emplace_back(
                pass.prevPostCopySemaphores
                    .at(i - 1)
                    .handle()
            );
        }

        VkPresentInfoKHR presentInfo{
            .sType =
                VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,

            .pNext =
                i == 0
                    ? pNext
                    : nullptr,

            .waitSemaphoreCount =
                static_cast<uint32_t>(
                    waitSemaphores.size()
                ),

            .pWaitSemaphores =
                waitSemaphores.data(),

            .swapchainCount = 1,

            .pSwapchains =
                &this->swapchain,

            .pImageIndices =
                &imageIdx,
        };

        res =
            Layer::ovkQueuePresentKHR(
                queue,
                &presentInfo
            );

        if (res != VK_SUCCESS &&
            res != VK_SUBOPTIMAL_KHR) {

            throw LSFG::vulkan_error(
                res,
                "Present failed"
            );
        }
    }

    /*
     * final original frame present
     */
    VkSemaphore lastSemaphore =
        pass.prevPostCopySemaphores.at(
            conf.multiplier - 2
        ).handle();

    VkPresentInfoKHR finalPresentInfo{
        .sType =
            VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,

        .waitSemaphoreCount = 1,

        .pWaitSemaphores =
            &lastSemaphore,

        .swapchainCount = 1,

        .pSwapchains =
            &this->swapchain,

        .pImageIndices =
            &presentIdx,
    };

    auto res =
        Layer::ovkQueuePresentKHR(
            queue,
            &finalPresentInfo
        );

    if (res != VK_SUCCESS &&
        res != VK_SUBOPTIMAL_KHR) {

        throw LSFG::vulkan_error(
            res,
            "Final present failed"
        );
    }

    this->frameIdx++;

    return res;
}
