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
/// One instance per swapchain.
///
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
    /* =====================================================
     * MODE FLAGS
     * ===================================================== */
    bool internalOnlyMode{false};   // LSFG external sync 실패 시 fallback
    bool lsfgEnabled{true};         // LSFG 정상 동작 여부

    /* =====================================================
     * SWAPCHAIN
     * ===================================================== */
    VkSwapchainKHR swapchain;
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent;

    /* =====================================================
     * LSFG CONTEXT
     * ===================================================== */
    std::shared_ptr<int32_t> lsfgCtxId;

    /* =====================================================
     * FRAME BUFFERS
     * ===================================================== */
    Mini::Image frame_0;
    Mini::Image frame_1;
    std::vector<Mini::Image> out_n;

    /* =====================================================
     * COMMAND SYSTEM
     * ===================================================== */
    Mini::CommandPool cmdPool;
    uint64_t frameIdx{0};

    /* =====================================================
     * PER-FRAME DATA
     * ===================================================== */
    struct RenderPassInfo {
        Mini::CommandBuffer preCopyBuf;

        std::array<Mini::Semaphore, 2> preCopySemaphores;

        std::vector<Mini::Semaphore> renderSemaphores;
        std::vector<Mini::Semaphore> acquireSemaphores;

        std::vector<Mini::CommandBuffer> postCopyBufs;
        std::vector<Mini::Semaphore> postCopySemaphores;
        std::vector<Mini::Semaphore> prevPostCopySemaphores;
    };

    std::array<RenderPassInfo, 8> passInfos;
};
