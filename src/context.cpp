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

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

LsContext::LsContext(
        const Hooks::DeviceInfo& info,
        VkSwapchainKHR swapchain,
        VkExtent2D extent,
        const std::vector<VkImage>& swapchainImages)
        : swapchain(swapchain),
          swapchainImages(swapchainImages),
          extent(extent) {

    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& globalConf = Config::globalConf;
    auto& conf = *Config::currentConf;

    const VkFormat format =
        conf.hdr
            ? VK_FORMAT_R8G8B8A8_UNORM
            : VK_FORMAT_R16G16B16A16_SFLOAT;

    /*
     * external memory fds
     */
    std::array<int, 2> fds{
        -1,
        -1
    };

    std::vector<int> outFdsRuntime(
        conf.multiplier - 1,
        -1
    );

    /*
     * create shared images
     */
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

    for (size_t i = 0; i < (conf.multiplier - 1); ++i) {

        this->out_n.emplace_back(
            info.device,
            info.physicalDevice,
            extent,
            format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &outFdsRuntime.at(i)
        );
    }

    std::cerr
        << "frame fd0 = "
        << fds.at(0)
        << std::endl;

    std::cerr
        << "frame fd1 = "
        << fds.at(1)
        << std::endl;

    for (size_t i = 0; i < outFdsRuntime.size(); ++i) {

        std::cerr
            << "out fd["
            << i
            << "] = "
            << outFdsRuntime.at(i)
            << std::endl;
    }

    /*
     * validate fd export
     */
    bool fdValid =
        (fds.at(0) >= 0) &&
        (fds.at(1) >= 0);

    for (auto fd : outFdsRuntime) {

        if (fd < 0) {
            fdValid = false;
            break;
        }
    }

    useShmFallback = !fdValid;

    std::cerr
        << "useShmFallback = "
        << useShmFallback
        << std::endl;

    /*
     * shm fallback
     */
    size_t shmSize =
        extent.width *
        extent.height *
        4;

    if (useShmFallback) {

        std::cerr
            << "creating shm fallback"
            << std::endl;

        for (int i = 0; i < 2; ++i) {

            std::string name =
                "/lsfg_shm_" +
                std::to_string(i);

            shm[i].fd =
                shm_open(
                    name.c_str(),
                    O_CREAT | O_RDWR,
                    0666
                );

            if (shm[i].fd < 0) {

                throw std::runtime_error(
                    "shm_open failed");
            }

            ftruncate(
                shm[i].fd,
                shmSize
            );

            shm[i].size = shmSize;

            shm[i].ptr =
                mmap(
                    nullptr,
                    shmSize,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    shm[i].fd,
                    0
                );

            if (shm[i].ptr == MAP_FAILED) {

                throw std::runtime_error(
                    "mmap failed");
            }

            /*
             * use shm fd as fake export fd
             */
            fds.at(i) = shm[i].fd;
        }

        for (size_t i = 0;
             i < outFdsRuntime.size();
             ++i) {

            outFdsRuntime.at(i) =
                dup(shm[0].fd);
        }
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

    setenv(
        "DISABLE_LSFG",
        "1",
        1
    );

    lsfgInitialize(
        Utils::getDeviceUUID(
            info.physicalDevice),
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

    for (auto fd : outFdsRuntime) {

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
                    outFdsRuntime,
                    extent,
                    format
                )
            ),
            [lsfgDeleteContext]
            (const int32_t* id) {

                lsfgDeleteContext(*id);
            }
        );

    unsetenv("DISABLE_LSFG");

    this->cmdPool =
        Mini::CommandPool(
            info.device,
            info.queue.first
        );

    for (size_t i = 0; i < 8; ++i) {

        auto& pass =
            this->passInfos.at(i);

        pass.renderSemaphores.resize(
            conf.multiplier - 1);

        pass.acquireSemaphores.resize(
            conf.multiplier - 1);

        pass.postCopyBufs.resize(
            conf.multiplier - 1);

        pass.postCopySemaphores.resize(
            conf.multiplier - 1);

        pass.prevPostCopySemaphores.resize(
            conf.multiplier - 1);
    }
}

void LsContext::uploadShmToGPU(
        const Hooks::DeviceInfo& info,
        void* src,
        Mini::Image& dst) {

    if (!src)
        return;

    size_t bufferSize =
        extent.width *
        extent.height *
        4;

    Mini::Buffer stagingBuffer(
        info.device,
        info.physicalDevice,
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    );

    void* mapped =
        stagingBuffer.map();

    memcpy(
        mapped,
        src,
        bufferSize
    );

    stagingBuffer.unmap();

    Mini::CommandBuffer cmd(
        info.device,
        this->cmdPool
    );

    cmd.begin();

    Utils::copyBufferToImage(
        cmd.handle(),
        stagingBuffer.handle(),
        dst.handle(),
        extent.width,
        extent.height
    );

    cmd.end();

    cmd.submit(
        info.queue.second,
        {},
        {}
    );
}
