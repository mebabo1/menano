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

enum class IPCMode {
    FD,
    SHM
};

LsContext::LsContext(const Hooks::DeviceInfo& info, VkSwapchainKHR swapchain,
        VkExtent2D extent, const std::vector<VkImage>& swapchainImages)
        : swapchain(swapchain),
          swapchainImages(swapchainImages),
          extent(extent) {

    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& globalConf = Config::globalConf;
    auto& conf = *Config::currentConf;

    const VkFormat format = conf.hdr
        ? VK_FORMAT_R8G8B8A8_UNORM
        : VK_FORMAT_R16G16B16A16_SFLOAT;

    std::array<int, 2> fds{};

    this->frame_0 = Mini::Image(
        info.device,
        info.physicalDevice,
        extent,
        format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
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
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds.at(1)
    );

    std::vector<int> outFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; ++i) {
        this->out_n.emplace_back(
            info.device,
            info.physicalDevice,
            extent,
            format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &outFds.at(i)
        );
    }

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

    this->ipcMode = IPCMode::FD;

    int ctx = lsfgCreateContext(
        fds.at(0),
        fds.at(1),
        outFds,
        extent,
        format
    );

    if (ctx < 0) {

        this->ipcMode = IPCMode::SHM;

        shmFrameSize = extent.width * extent.height * 4;

        std::string base = "/data/data/com.termux/files/usr/tmp/lsfg_";

        for (int i = 0; i < conf.multiplier - 1; i++) {

            std::string inName = base + "in_" + std::to_string(i);
            std::string outName = base + "out_" + std::to_string(i);

            int shmIn = shm_open(inName.c_str(), O_CREAT | O_RDWR, 0666);
            int shmOut = shm_open(outName.c_str(), O_CREAT | O_RDWR, 0666);

            ftruncate(shmIn, shmFrameSize);
            ftruncate(shmOut, shmFrameSize);

            void* inPtr = mmap(
                nullptr,
                shmFrameSize,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                shmIn,
                0
            );

            void* outPtr = mmap(
                nullptr,
                shmFrameSize,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                shmOut,
                0
            );

            shmInputs.push_back(inPtr);
            shmOutputs.push_back(outPtr);
        }

        ctx = lsfgCreateContext(0, 0, {}, extent, format);
    }

    this->lsfgCtxId = std::shared_ptr<int32_t>(
        new int32_t(ctx),
        [lsfgDeleteContext](const int32_t* id) {
            lsfgDeleteContext(*id);
        }
    );

    unsetenv("DISABLE_LSFG");

    this->cmdPool = Mini::CommandPool(
        info.device,
        info.queue.first
    );

    for (size_t i = 0; i < 8; i++) {
        auto& pass = this->passInfos.at(i);

        pass.renderSemaphores.resize(conf.multiplier - 1);
        pass.acquireSemaphores.resize(conf.multiplier - 1);
        pass.postCopyBufs.resize(conf.multiplier - 1);
        pass.postCopySemaphores.resize(conf.multiplier - 1);
        pass.prevPostCopySemaphores.resize(conf.multiplier - 1);
    }
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

    auto& pass = this->passInfos.at(this->frameIdx % 8);

    int preCopySemaphoreFd{};

    pass.preCopySemaphores.at(0) =
        Mini::Semaphore(info.device, &preCopySemaphoreFd);

    pass.preCopySemaphores.at(1) =
        Mini::Semaphore(info.device);

    pass.preCopyBuf =
        Mini::CommandBuffer(info.device, this->cmdPool);

    pass.preCopyBuf.begin();

    Utils::copyImage(
        pass.preCopyBuf.handle(),
        this->swapchainImages.at(presentIdx),
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

    std::vector<VkSemaphore> preWaits = gameRenderSemaphores;

    if (this->frameIdx > 0) {
        preWaits.emplace_back(
            this->passInfos.at((this->frameIdx - 1) % 8)
                .preCopySemaphores.at(1)
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

    std::vector<int> renderSemaphoreFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; ++i) {
        pass.renderSemaphores.at(i) =
            Mini::Semaphore(info.device, &renderSemaphoreFds.at(i));
    }

    if (this->ipcMode == IPCMode::FD) {

        LSFG_3_1::presentContext(
            *this->lsfgCtxId,
            preCopySemaphoreFd,
            renderSemaphoreFds
        );

    } else {

        LSFG_3_1::presentContext(
            *this->lsfgCtxId,
            0,
            renderSemaphoreFds
        );

        for (size_t i = 0; i < conf.multiplier - 1; i++) {

            memcpy(
                shmInputs[i],
                this->frameIdx % 2 == 0
                    ? this->frame_0.handle()
                    : this->frame_1.handle(),
                shmFrameSize
            );
        }
    }

    for (size_t i = 0; i < conf.multiplier - 1; i++) {

        pass.acquireSemaphores.at(i) =
            Mini::Semaphore(info.device);

        uint32_t imageIdx{};

        auto res = Layer::ovkAcquireNextImageKHR(
            info.device,
            this->swapchain,
            UINT64_MAX,
            pass.acquireSemaphores.at(i).handle(),
            VK_NULL_HANDLE,
            &imageIdx
        );

        if (res != VK_SUCCESS &&
            res != VK_SUBOPTIMAL_KHR) {
            throw LSFG::vulkan_error(res, "Failed to acquire next swapchain image");
        }

        pass.postCopyBufs.at(i) =
            Mini::CommandBuffer(info.device, this->cmdPool);

        pass.postCopyBufs.at(i).begin();

        if (this->ipcMode == IPCMode::FD) {

            Utils::copyImage(
                pass.postCopyBufs.at(i).handle(),
                this->out_n.at(i).handle(),
                this->swapchainImages.at(imageIdx),
                this->extent.width,
                this->extent.height,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                false,
                true
            );

        } else {

            Utils::copyImage(
                pass.postCopyBufs.at(i).handle(),
                this->frame_0.handle(), // staging path placeholder
                this->swapchainImages.at(imageIdx),
                this->extent.width,
                this->extent.height,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                false,
                true
            );

            memcpy(
                this->swapchainImages.data(),
                shmOutputs[i],
                shmFrameSize
            );
        }

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

        std::vector<VkSemaphore> waitSemaphores{
            pass.postCopySemaphores.at(i).handle()
        };

        if (i != 0) {
            waitSemaphores.emplace_back(
                pass.prevPostCopySemaphores.at(i - 1).handle()
            );
        }

        VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = i == 0 ? pNext : nullptr,
            .waitSemaphoreCount = (uint32_t)waitSemaphores.size(),
            .pWaitSemaphores = waitSemaphores.data(),
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &imageIdx,
        };

        res = Layer::ovkQueuePresentKHR(queue, &presentInfo);

        if (res != VK_SUCCESS &&
            res != VK_SUBOPTIMAL_KHR) {
            throw LSFG::vulkan_error(res, "Failed to present generated frame");
        }
    }

    VkSemaphore lastSemaphore =
        pass.prevPostCopySemaphores.at(conf.multiplier - 2).handle();

    VkPresentInfoKHR finalPresentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &lastSemaphore,
        .swapchainCount = 1,
        .pSwapchains = &this->swapchain,
        .pImageIndices = &presentIdx,
    };

    auto res = Layer::ovkQueuePresentKHR(queue, &finalPresentInfo);

    if (res != VK_SUCCESS &&
        res != VK_SUBOPTIMAL_KHR) {
        throw LSFG::vulkan_error(res, "Failed to present real frame");
    }

    this->frameIdx++;

    return res;
}
