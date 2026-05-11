#include <iostream>
#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/device.hpp"
#include "core/instance.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace LSFG::Core;

/*
 * Android / Termux safe extension set
 *
 * Keep:
 * - external fd memory
 * - external fd semaphore
 * - sync2
 * - timeline semaphore support
 *
 * Remove:
 * - robustness2 dependency
 * - AHB dependency
 */
const std::vector<const char*> requiredExtensions = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME
};

Device::Device(
        const Instance& instance,
        uint64_t deviceUUID,
        bool forceDisableFp16) {

    /*
     * Enumerate physical devices
     */
    uint32_t deviceCount{};

    auto res = vkEnumeratePhysicalDevices(
        instance.handle(),
        &deviceCount,
        nullptr);

    if (res != VK_SUCCESS || deviceCount == 0) {
        throw LSFG::vulkan_error(
            res,
            "Failed to enumerate physical devices");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);

    res = vkEnumeratePhysicalDevices(
        instance.handle(),
        &deviceCount,
        devices.data());

    if (res != VK_SUCCESS) {
        throw LSFG::vulkan_error(
            res,
            "Failed to get physical devices");
    }

    /*
     * Select device by UUID
     */
    std::optional<VkPhysicalDevice> physicalDevice;

    for (const auto& device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);

        const uint64_t uuid =
            (static_cast<uint64_t>(properties.vendorID) << 32)
            | properties.deviceID;

        std::cerr
            << "Detected GPU vendor="
            << properties.vendorID
            << " device="
            << properties.deviceID
            << std::endl;

        if (deviceUUID == uuid) {
            physicalDevice = device;

            std::cerr
                << "Matched target GPU"
                << std::endl;

            break;
        }
    }

    /*
     * Fallback to first GPU
     * (important for Wine/Turnip edge cases)
     */
    if (!physicalDevice && !devices.empty()) {
        std::cerr
            << "Falling back to first Vulkan GPU"
            << std::endl;

        physicalDevice = devices[0];
    }

    if (!physicalDevice) {
        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "Could not find physical device");
    }

    /*
     * Queue family selection
     *
     * Prefer graphics+compute unified queue.
     * Turnip behaves better with unified queues.
     */
    uint32_t familyCount{};

    vkGetPhysicalDeviceQueueFamilyProperties(
        *physicalDevice,
        &familyCount,
        nullptr);

    std::vector<VkQueueFamilyProperties>
        queueFamilies(familyCount);

    vkGetPhysicalDeviceQueueFamilyProperties(
        *physicalDevice,
        &familyCount,
        queueFamilies.data());

    std::optional<uint32_t> computeFamilyIdx;

    /*
     * Prefer graphics+compute queue
     */
    for (uint32_t i = 0; i < familyCount; ++i) {
        const auto flags = queueFamilies[i].queueFlags;

        if ((flags & VK_QUEUE_GRAPHICS_BIT) &&
            (flags & VK_QUEUE_COMPUTE_BIT)) {

            computeFamilyIdx = i;
            break;
        }
    }

    /*
     * Fallback to compute-only queue
     */
    if (!computeFamilyIdx) {
        for (uint32_t i = 0; i < familyCount; ++i) {
            if (queueFamilies[i].queueFlags &
                VK_QUEUE_COMPUTE_BIT) {

                computeFamilyIdx = i;
                break;
            }
        }
    }

    if (!computeFamilyIdx) {
        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "No compute queue family found");
    }

    std::cerr
        << "Selected queue family = "
        << *computeFamilyIdx
        << std::endl;

    /*
     * Probe Vulkan 1.2 features
     */
    VkPhysicalDeviceVulkan12Features
        supported12Features{
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
        };

    VkPhysicalDeviceFeatures2 supportedFeatures{
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &supported12Features
    };

    vkGetPhysicalDeviceFeatures2(
        *physicalDevice,
        &supportedFeatures);

    /*
     * FP16 support
     */
    this->supportsFP16 =
        !forceDisableFp16 &&
        supported12Features.shaderFloat16;

    if (this->supportsFP16) {
        std::cerr
            << "lsfg-vk: Using FP16 acceleration"
            << std::endl;
    } else {
        std::cerr
            << "lsfg-vk: Using FP32 path"
            << std::endl;
    }

    /*
     * Queue creation
     */
    const float queuePriority = 1.0f;

    const VkDeviceQueueCreateInfo
        computeQueueDesc{
            .sType =
                VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = *computeFamilyIdx,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority
        };

    /*
     * Vulkan 1.3 features
     *
     * Keep synchronization2 enabled.
     */
    VkPhysicalDeviceVulkan13Features
        features13{
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .synchronization2 = VK_TRUE
        };

    /*
     * Vulkan 1.2 features
     */
    const VkPhysicalDeviceVulkan12Features
        features12{
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = &features13,

            .shaderFloat16 =
                this->supportsFP16,

            .timelineSemaphore =
                VK_TRUE,

            .vulkanMemoryModel =
                VK_TRUE
        };

    /*
     * Device creation
     */
    const VkDeviceCreateInfo
        deviceCreateInfo{
            .sType =
                VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,

            .pNext = &features12,

            .queueCreateInfoCount = 1,
            .pQueueCreateInfos =
                &computeQueueDesc,

            .enabledExtensionCount =
                static_cast<uint32_t>(
                    requiredExtensions.size()),

            .ppEnabledExtensionNames =
                requiredExtensions.data()
        };

    VkDevice deviceHandle{};

    res = vkCreateDevice(
        *physicalDevice,
        &deviceCreateInfo,
        nullptr,
        &deviceHandle);

    std::cerr
        << "vkCreateDevice result = "
        << res
        << std::endl;

    /*
     * IMPORTANT:
     * use logical OR (||)
     * NOT bitwise OR (|)
     */
    if (res != VK_SUCCESS ||
        deviceHandle == VK_NULL_HANDLE) {

        throw LSFG::vulkan_error(
            res,
            "Failed to create logical device");
    }

    /*
     * Load Vulkan device symbols
     */
    volkLoadDevice(deviceHandle);

    /*
     * Get compute queue
     */
    VkQueue queueHandle{};

    vkGetDeviceQueue(
        deviceHandle,
        *computeFamilyIdx,
        0,
        &queueHandle);

    /*
     * Store handles
     */
    this->computeQueue = queueHandle;
    this->computeFamilyIdx = *computeFamilyIdx;
    this->physicalDevice = *physicalDevice;

    this->device = std::shared_ptr<VkDevice>(
        new VkDevice(deviceHandle),
        [](VkDevice* device) {
            vkDestroyDevice(*device, nullptr);
        }
    );
}
