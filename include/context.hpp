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
    LsContext(const Hooks::DeviceInfo& info, VkSwapchainKHR swapchain,
        VkExtent2D extent, const std::vector<VkImage>& swapchainImages);

    VkResult present(const Hooks::DeviceInfo& info, const void* pNext, VkQueue queue,
        const std::vector<VkSemaphore>& gameRenderSemaphores, uint32_t presentIdx);

    LsContext(const LsContext&) = delete;
    LsContext& operator=(const LsContext&) = delete;
    LsContext(LsContext&&) = default;
    LsContext& operator=(LsContext&&) = default;
    ~LsContext() = default;

private:
    enum class IPCMode {
        FD,
        SHM
    };

    struct ShmBuffer {
        int fd = -1;
        void* ptr = nullptr;
        size_t size = 0;
    };

    struct RenderPassInfo {
        Mini::CommandBuffer preCopyBuf;
        std::array<Mini::Semaphore, 2> preCopySemaphores;

        std::vector<Mini::Semaphore> renderSemaphores;
        std::vector<Mini::Semaphore> acquireSemaphores;

        std::vector<Mini::CommandBuffer> postCopyBufs;
        std::vector<Mini::Semaphore> postCopySemaphores;
        std::vector<Mini::Semaphore> prevPostCopySemaphores;
    };

private:
    VkSwapchainKHR swapchain;
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent;

    std::shared_ptr<int32_t> lsfgCtxId;

    Mini::Image frame_0, frame_1;
    std::vector<Mini::Image> out_n;

    Mini::CommandPool cmdPool;
    uint64_t frameIdx{0};

    VkDevice device{};
    VkPhysicalDevice physicalDevice{};
    VkQueue queue{};

    bool useShmFallback{false};
    IPCMode ipcMode{IPCMode::FD};

    size_t shmFrameSize{0};

    ShmBuffer shm[2];

    std::vector<void*> shmInputs;
    std::vector<void*> shmOutputs;

    bool lsfgInitialized{false};

    void uploadShmToGPU(void* src, Mini::Image& dst);

    std::array<RenderPassInfo, 8> passInfos;
};
