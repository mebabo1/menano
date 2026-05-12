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

    std::array<int, 2> fds{ -1, -1 };

    frame_0 = Mini::Image(
        info.device, info.physicalDevice, extent, format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds[0]
    );

    frame_1 = Mini::Image(
        info.device, info.physicalDevice, extent, format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds[1]
    );

    std::vector<int> outFds(conf.multiplier - 1);

    for (size_t i = 0; i < conf.multiplier - 1; i++) {
        out_n.emplace_back(
            info.device, info.physicalDevice, extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &outFds[i]
        );
    }

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

    ipcMode = IPCMode::FD;

    int ctx = create(
        fds[0],
        fds[1],
        outFds,
        extent,
        format
    );

    if (ctx < 0) {

        ipcMode = IPCMode::SHM;

        shmFrameSize = extent.width * extent.height * 4;

        std::string base = "/data/data/com.termux/files/usr/tmp/lsfg_";

        shmInputs.clear();
        shmOutputs.clear();

        for (int i = 0; i < conf.multiplier - 1; i++) {

            std::string inName = base + "in_" + std::to_string(i);
            std::string outName = base + "out_" + std::to_string(i);

            int shmIn = shm_open(inName.c_str(), O_CREAT | O_RDWR, 0666);
            int shmOut = shm_open(outName.c_str(), O_CREAT | O_RDWR, 0666);

            if (shmIn < 0 || shmOut < 0)
                throw std::runtime_error("shm_open failed");

            if (ftruncate(shmIn, shmFrameSize) != 0 ||
                ftruncate(shmOut, shmFrameSize) != 0)
                throw std::runtime_error("ftruncate failed");

            void* inPtr = mmap(nullptr, shmFrameSize,
                PROT_READ | PROT_WRITE, MAP_SHARED, shmIn, 0);

            void* outPtr = mmap(nullptr, shmFrameSize,
                PROT_READ | PROT_WRITE, MAP_SHARED, shmOut, 0);

            if (inPtr == MAP_FAILED || outPtr == MAP_FAILED)
                throw std::runtime_error("mmap failed");

            shmInputs.push_back(inPtr);
            shmOutputs.push_back(outPtr);
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

    for (size_t i = 0; i < 8; i++) {
        auto& pass = passInfos[i];

        pass.renderSemaphores.resize(conf.multiplier - 1);
        pass.acquireSemaphores.resize(conf.multiplier - 1);
        pass.postCopyBufs.resize(conf.multiplier - 1);
        pass.postCopySemaphores.resize(conf.multiplier - 1);
        pass.prevPostCopySemaphores.resize(conf.multiplier - 1);
    }
        }
