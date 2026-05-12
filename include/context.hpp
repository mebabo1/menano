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
    LsContext(const Hooks::DeviceInfo& info, VkSwapchainKHR swapchain,
        VkExtent2D extent, const std::vector<VkImage>& swapchainImages);

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
    VkSwapchainKHR swapchain;
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent;

    std::shared_ptr<int32_t> lsfgCtxId;

    Mini::Image frame_0, frame_1;
    std::vector<Mini::Image> out_n;

    Mini::CommandPool cmdPool;
    uint64_t frameIdx{0};

    ///
    /// IPC mode (FD or SHM fallback)
    ///
    IPCMode ipcMode{IPCMode::FD};

    ///
    /// SHM buffers (fallback path)
    ///
    std::vector<void*> shmInputs;
    std::vector<void*> shmOutputs;
    size_t shmFrameSize{0};

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
