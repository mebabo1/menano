#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <cstddef>
#include <utility>
#include <string>
#include <vector>

namespace Utils {

    std::pair<uint32_t, VkQueue> findQueue(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        VkDeviceCreateInfo* desc,
        VkQueueFlags flags
    );

    uint64_t getDeviceUUID(VkPhysicalDevice physicalDevice);

    uint32_t getMaxImageCount(
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface
    );

    std::vector<const char*> addExtensions(
        const char* const* extensions,
        size_t count,
        const std::vector<const char*>& requiredExtensions
    );

    void copyImage(
        VkCommandBuffer buf,
        VkImage src,
        VkImage dst,
        uint32_t width,
        uint32_t height,
        VkPipelineStageFlags pre,
        VkPipelineStageFlags post,
        bool makeSrcPresentable,
        bool makeDstPresentable
    );

    void logLimitN(
        const std::string& id,
        size_t n,
        const std::string& message
    );

    void resetLimitN(const std::string& id) noexcept;

    std::pair<std::string, std::string> getProcessName();

    std::string getConfigFile();

}
