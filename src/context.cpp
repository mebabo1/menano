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

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#include <linux/memfd.h>
#include <sys/syscall.h>

static int createSharedFd(size_t size) {

    int fd = syscall(
        SYS_memfd_create,
        "lsfg_gpu_fallback",
        MFD_CLOEXEC
    );

    if (fd < 0)
        return -1;

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

LsContext::LsContext(
        const Hooks::DeviceInfo& info,
        VkSwapchainKHR swapchain,
        VkExtent2D extent,
        const std::vector<VkImage>& swapchainImages)
        : swapchain(swapchain),
          swapchainImages(swapchainImages),
          extent(extent),
          info(info) {

    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& globalConf = Config::globalConf;
    auto& conf = *Config::currentConf;

    const VkFormat format = conf.hdr
        ? VK_FORMAT_R16G16B16A16_SFLOAT
        : VK_FORMAT_R8G8B8A8_UNORM;

    size_t shmSize =
        extent.width *
        extent.height *
        4;

    /*
     * create fallback shared fds
     */

    std::array<int, 2> fds{
        createSharedFd(shmSize),
        createSharedFd(shmSize)
    };

    std::vector<int> outFds(
        conf.multiplier - 1,
        -1
    );

    for (size_t i = 0; i < outFds.size(); ++i) {
        outFds[i] = createSharedFd(shmSize);
    }

    bool fdValid =
        (fds[0] >= 0) &&
        (fds[1] >= 0);

    for (auto fd : outFds)
        fdValid &= (fd >= 0);

    useShmFallback = !fdValid;

    std::cerr
        << "frame fd0 = "
        << fds[0]
        << std::endl;

    std::cerr
        << "frame fd1 = "
        << fds[1]
        << std::endl;

    if (!fdValid) {

        std::cerr
            << "lsfg-vk: shared fd fallback failed"
            << std::endl;

        for (int i = 0; i < 2; i++) {

            shm[i].fd =
                shm_open(
                    "/lsfg_shm",
                    O_CREAT | O_RDWR,
                    0666
                );

            ftruncate(shm[i].fd, shmSize);

            shm[i].size = shmSize;

            shm[i].ptr = mmap(
                nullptr,
                shmSize,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                shm[i].fd,
                0
            );
        }
    }

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

    /*
     * create generated output images
     */

    std::vector<int> outFdsRuntime = outFds;

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
            &outFdsRuntime.at(i)
        );

        std::cerr
            << "out fd[" << i << "] = "
            << outFdsRuntime.at(i)
            << std::endl;
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
            info.physicalDevice),
        conf.hdr,
        1.0F / conf.flowScale,
        conf.multiplier - 1,
        globalConf.no_fp16,
        Extract::getShader
    );

    int fd0 =
        fdValid ? fds.at(0) : -1;

    int fd1 =
        fdValid ? fds.at(1) : -1;

    std::cerr
        << "createContext fd0="
        << fd0
        << " fd1="
        << fd1
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
                    fd0,
                    fd1,
                    outFdsRuntime,
                    extent,
                    format
                )
            ),
            [lsfgDeleteContext =
                lsfgDeleteContext](
                    const int32_t* id) {

                lsfgDeleteContext(*id);
            }
        );

    unsetenv("DISABLE_LSFG");

    this->cmdPool =
        Mini::CommandPool(
            info.device,
            info.queue.first
        );

    for (size_t i = 0; i < 8; i++) {

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
