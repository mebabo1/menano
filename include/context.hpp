#pragma once

#include "hooks.hpp"
#include "mini/commandbuffer.hpp"
#include "mini/commandpool.hpp"
#include "mini/image.hpp"
#include "mini/semaphore.hpp"

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class LsContext {
public:

    LsContext(
        const Hooks::DeviceInfo& info,
        VkSwapchainKHR swapchain,
        VkExtent2D extent,
        const std::vector<VkImage>& swapchainImages
    );

    VkResult present(
        const Hooks::DeviceInfo& info,
        const void* pNext,
        VkQueue queue,
        const std::vector<VkSemaphore>& gameRenderSemaphores,
        uint32_t presentIdx
    );

    LsContext(const LsContext&) = delete;
    LsContext& operator=(const LsContext&) = delete;

    LsContext(LsContext&&) = default;
    LsContext& operator=(LsContext&&) = default;

    ~LsContext() = default;

private:

    /*
     * persistent device info
     */
    Hooks::DeviceInfo info;

    /*
     * swapchain
     */
    VkSwapchainKHR swapchain;

    std::vector<VkImage> swapchainImages;

    VkExtent2D extent;

    /*
     * lsfg context id
     */
    std::shared_ptr<int32_t> lsfgCtxId;

    /*
     * input frames shared with LSFG
     */
    Mini::Image frame_0;
    Mini::Image frame_1;

    /*
     * generated output frames
     */
    std::vector<Mini::Image> out_n;

    /*
     * command resources
     */
    Mini::CommandPool cmdPool;

    uint64_t frameIdx{0};

    struct RenderPassInfo {

        /*
         * swapchain -> frame copy
         */
        Mini::CommandBuffer preCopyBuf;

        /*
         * pre-copy sync
         */
        std::array<Mini::Semaphore, 2>
            preCopySemaphores;

        /*
         * LSFG render completion
         */
        std::vector<Mini::Semaphore>
            renderSemaphores;

        /*
         * swapchain acquire
         */
        std::vector<Mini::Semaphore>
            acquireSemaphores;

        /*
         * output -> swapchain copy
         */
        std::vector<Mini::CommandBuffer>
            postCopyBufs;

        /*
         * post-copy sync
         */
        std::vector<Mini::Semaphore>
            postCopySemaphores;

        /*
         * ordering sync
         */
        std::vector<Mini::Semaphore>
            prevPostCopySemaphores;
    };

    /*
     * frames in flight
     */
    std::array<RenderPassInfo, 8>
        passInfos;
};
