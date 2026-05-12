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

///
/// This class is the frame generation context.
/// There should be one instance per swapchain.
///
class LsContext {
public:

    ///
    /// Create the swapchain context.
    ///
    /// @param info The device information to use.
    /// @param swapchain The Vulkan swapchain to use.
    /// @param extent The extent of the swapchain images.
    /// @param swapchainImages The swapchain images to use.
    ///
    /// @throws LSFG::vulkan_error if any Vulkan call fails.
    ///
    LsContext(
        const Hooks::DeviceInfo& info,
        VkSwapchainKHR swapchain,
        VkExtent2D extent,
        const std::vector<VkImage>& swapchainImages
    );

    ///
    /// Custom present logic.
    ///
    /// @param info The device information to use.
    /// @param pNext Unknown pointer set in the present info structure.
    /// @param queue The Vulkan queue to present the frame on.
    /// @param gameRenderSemaphores The semaphores to wait on before presenting.
    /// @param presentIdx The index of the swapchain image to present.
    ///
    /// @return The result of the Vulkan present operation.
    ///
    /// @throws LSFG::vulkan_error if any Vulkan call fails.
    ///
    VkResult present(
        const Hooks::DeviceInfo& info,
        const void* pNext,
        VkQueue queue,
        const std::vector<VkSemaphore>& gameRenderSemaphores,
        uint32_t presentIdx
    );

    // Non-copyable
    LsContext(const LsContext&) = delete;
    LsContext& operator=(const LsContext&) = delete;

    // Moveable
    LsContext(LsContext&&) = default;
    LsContext& operator=(LsContext&&) = default;

    ~LsContext() = default;

private:

    struct ShmBuffer {
        int fd{-1};
        void* ptr{nullptr};
        size_t size{0};
    };

    /*
     * helper:
     * upload shared memory to gpu image
     */
    void uploadShmToGPU(
        void* src,
        Mini::Image& dst
    );

private:

    Hooks::DeviceInfo info;

    VkSwapchainKHR swapchain;

    std::vector<VkImage> swapchainImages;

    VkExtent2D extent;

    bool useShmFallback{false};

    std::array<ShmBuffer, 2> shm;

    /*
     * lsfg context id
     */
    std::shared_ptr<int32_t> lsfgCtxId;

    /*
     * frames shared with lsfg
     */
    Mini::Image frame_0;
    Mini::Image frame_1;

    /*
     * output images shared with lsfg
     */
    std::vector<Mini::Image> out_n;

    Mini::CommandPool cmdPool;

    uint64_t frameIdx{0};

    struct RenderPassInfo {

        /*
         * copy swapchain -> frame_0/frame_1
         */
        Mini::CommandBuffer preCopyBuf;

        /*
         * signal pre-copy done
         */
        std::array<Mini::Semaphore, 2>
            preCopySemaphores;

        /*
         * signal lsfg done
         */
        std::vector<Mini::Semaphore>
            renderSemaphores;

        /*
         * acquire semaphores
         */
        std::vector<Mini::Semaphore>
            acquireSemaphores;

        /*
         * copy generated frame -> swapchain
         */
        std::vector<Mini::CommandBuffer>
            postCopyBufs;

        /*
         * post-copy completion
         */
        std::vector<Mini::Semaphore>
            postCopySemaphores;

        /*
         * ordering between presents
         */
        std::vector<Mini::Semaphore>
            prevPostCopySemaphores;
    };

    /*
     * allocate 8 frames-in-flight
     */
    std::array<RenderPassInfo, 8>
        passInfos;
};
