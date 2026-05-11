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

LsContext::LsContext(const Hooks::DeviceInfo& info, VkSwapchainKHR swapchain,
        VkExtent2D extent, const std::vector<VkImage>& swapchainImages)
        : swapchain(swapchain), swapchainImages(swapchainImages),
          extent(extent) {

    if (!Config::currentConf.has_value())
        throw std::runtime_error("No configuration set");

    auto& globalConf = Config::globalConf;
    auto& conf = *Config::currentConf;

    /*
     * TERMUX / ANDROID SAFE FORMAT
     *
     * FP16 external memory import/export is unstable
     * on many Android Vulkan drivers.
     *
     * Force RGBA8.
     */
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    // prepare textures for lsfg
    std::array<int, 2> fds{};

    this->frame_0 = Mini::Image(
        info.device,
        info.physicalDevice,
        extent,
        format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds.at(0)
    );

    this->frame_1 = Mini::Image(
        info.device,
        info.physicalDevice,
        extent,
        format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        &fds.at(1)
    );

    std::vector<int> outFds(conf.multiplier - 1);

    for (size_t i = 0; i < (conf.multiplier - 1); ++i) {
        this->out_n.emplace_back(
            info.device,
            info.physicalDevice,
            extent,
            format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &outFds.at(i)
        );
    }

    // initialize lsfg
    auto* lsfgInitialize = LSFG_3_1::initialize;
    auto* lsfgCreateContext = LSFG_3_1::createContext;
    auto* lsfgDeleteContext = LSFG_3_1::deleteContext;

    if (conf.performance) {
        lsfgInitialize = LSFG_3_1P::initialize;
        lsfgCreateContext = LSFG_3_1P::createContext;
        lsfgDeleteContext = LSFG_3_1P::deleteContext;
    }

    setenv("DISABLE_LSFG", "1", 1);

    /*
     * FORCE FP16 OFF
     *
     * Android Vulkan external image interop
     * often crashes with shaderFloat16 paths.
     */
    lsfgInitialize(
        Utils::getDeviceUUID(info.physicalDevice),
        false, // force HDR OFF
        1.0F / conf.flowScale,
        conf.multiplier - 1,
        true, // forceDisableFp16 = true
        Extract::getShader
    );

    this->lsfgCtxId = std::shared_ptr<int32_t>(
        new int32_t(
            lsfgCreateContext(
                fds.at(0),
                fds.at(1),
                outFds,
                extent,
                format
            )
        ),
        [lsfgDeleteContext = lsfgDeleteContext](const int32_t* id) {
            lsfgDeleteContext(*id);
        }
    );

    unsetenv("DISABLE_LSFG");

    // prepare render passes
    this->cmdPool = Mini::CommandPool(info.device, info.queue.first);

    for (size_t i = 0; i < 8; i++) {
        auto& pass = this->passInfos.at(i);

        pass.renderSemaphores.resize(conf.multiplier - 1);
        pass.acquireSemaphores.resize(conf.multiplier - 1);
        pass.postCopyBufs.resize(conf.multiplier - 1);
        pass.postCopySemaphores.resize(conf.multiplier - 1);
        pass.prevPostCopySemaphores.resize(conf.multiplier - 1);
    }
        }
