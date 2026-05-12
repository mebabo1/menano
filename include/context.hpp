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
#include <string>

///
/// IPC fallback mode
///
enum class IPCMode {
    FD,
    SHM
};

///
/// Frame generation context (one per swapchain)
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

    LsContext(LsContext&&) noexcept = default;
    LsContext& operator=(LsContext&&) noexcept = default;

    ~LsContext() = default;

private:
    // =========================
    // Vulkan swapchain state
    // =========================
    VkSwapchainKHR swapchain{};
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent{};

    // =========================
    // LSFG runtime
    // =========================
    std::shared_ptr<int32_t> lsfgCtxId;

    // =========================
    // GPU images (ping-pong)
    // =========================
    Mini::Image frame_0;
    Mini::Image frame_1;
    std::vector<Mini::Image> out_n;

    // =========================
    // Command system
    // =========================
    Mini::CommandPool cmdPool;
    uint64_t frameIdx{0};

    // =========================
    // IPC mode
    // =========================
    IPCMode ipcMode{IPCMode::FD};

    // =========================
    // SHM fallback
    // =========================
    std::vector<void*> shmInputs;
    std::vector<void*> shmOutputs;
    size_t shmFrameSize{0};

    // =========================
    // Frame pipeline state
    // =========================
    struct RenderPassInfo {
        Mini::CommandBuffer preCopyBuf;

        std::array<Mini::Semaphore, 2> preCopySemaphores;

        std::vector<Mini::Semaphore> renderSemaphores;
        std::vector<Mini::Semaphore> acquireSemaphores;

        std::vector<Mini::CommandBuffer> postCopyBufs;

        // IMPORTANT:
        // store handles, NOT .handle() temp usage
        std::vector<VkSemaphore> postCopySemaphores;
        std::vector<VkSemaphore> prevPostCopySemaphores;
    };

    std::array<RenderPassInfo, 8> passInfos;
};
