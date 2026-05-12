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

    // =========================
    // INPUT (FD SURFACES)
    // =========================
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

    // =========================
    // OUTPUT
    // =========================
    std::vector<int> outFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; i++) {
        out_n.emplace_back(info.device, info.physicalDevice, extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &outFds[i]);
    }

    // =========================
    // LSFG INIT
    // =========================
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

    // =========================
    // SHM FALLBACK
    // =========================
    if (ctx < 0) {

        ipcMode = IPCMode::SHM;
        fdFailed = true;

        shmFrameSize = extent.width * extent.height * 4;

        std::string base = "/data/data/com.termux/files/usr/tmp/lsfg_";

        for (int i = 0; i < conf.multiplier - 1; i++) {

            std::string inName  = base + "in_"  + std::to_string(i);
            std::string outName = base + "out_" + std::to_string(i);

            int shmIn  = shm_open(inName.c_str(),  O_CREAT | O_RDWR, 0666);
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

    for (auto& pass : passInfos) {
        pass.renderSemaphores.resize(conf.multiplier - 1);
        pass.acquireSemaphores.resize(conf.multiplier - 1);
        pass.postCopyBufs.resize(conf.multiplier - 1);
        pass.postCopySemaphores.resize(conf.multiplier - 1);
        pass.prevPostCopySemaphores.resize(conf.multiplier - 1);
    }
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

    // =========================
    // PRE COPY
    // =========================
    int preCopySemaphoreFd{};

    pass.preCopySemaphores[0] =
        Mini::Semaphore(info.device, &preCopySemaphoreFd);

    pass.preCopySemaphores[1] =
        Mini::Semaphore(info.device);

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
        {
            pass.preCopySemaphores[0].handle(),
            pass.preCopySemaphores[1].handle()
        }
    );

    // =========================
    // LSFG CALL
    // =========================
    std::vector<int> renderFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; i++) {
        pass.renderSemaphores[i] =
            Mini::Semaphore(info.device, &renderFds[i]);
    }

    bool useFdPath = (!fdFailed && ipcMode == IPCMode::FD && lsfgCtxId);

    if (useFdPath) {

        LSFG_3_1::presentContext(*lsfgCtxId, preCopySemaphoreFd, renderFds);

    } else {

        // =========================
        // SHM → GPU BRIDGE
        // =========================

        std::vector<uint8_t> cpu(shmFrameSize);

        memcpy(cpu.data(), shmOutputs[0], shmFrameSize);

        Utils::uploadBufferToImage(
            info.device,
            cpu.data(),
            frameIdx % 2 == 0 ? frame_0.handle() : frame_1.handle(),
            extent.width,
            extent.height
        );

        LSFG_3_1::presentContext(*lsfgCtxId, preCopySemaphoreFd, renderFds);
    }

    // =========================
    // PRESENT
    // =========================
    pass.acquireSemaphores[0] = Mini::Semaphore(info.device);

    uint32_t imageIdx{};

    Layer::ovkAcquireNextImageKHR(
        info.device,
        swapchain,
        UINT64_MAX,
        pass.acquireSemaphores[0].handle(),
        VK_NULL_HANDLE,
        &imageIdx
    );

    pass.postCopyBufs[0] = Mini::CommandBuffer(info.device, cmdPool);
    pass.postCopyBufs[0].begin();

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

    pass.postCopyBufs[0].end();

    pass.postCopySemaphores[0] = Mini::Semaphore(info.device);

    pass.postCopyBufs[0].submit(
        info.queue.second,
        {
            pass.acquireSemaphores[0].handle(),
            pass.renderSemaphores[0].handle()
        },
        {
            pass.postCopySemaphores[0].handle()
        }
    );

    VkSemaphore waitSem = pass.postCopySemaphores[0].handle();

    VkPresentInfoKHR pi{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = pNext,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &waitSem,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIdx,
    };

    Layer::ovkQueuePresentKHR(queue, &pi);

    frameIdx++;
    return VK_SUCCESS;
}
