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
 * Minimal required extensions (clean & correct)
 */
const std::vector<const char*> requiredExtensions = {
    "VK_KHR_swapchain",
    "VK_KHR_external_memory_fd",
    "VK_KHR_external_semaphore_fd"
};

Device::Device(
    const Instance& instance,
    uint64_t deviceUUID,
    bool forceDisableFp16)
{
    /* -----------------------------------------------------
     * 1. Enumerate physical devices
     * ----------------------------------------------------- */
    uint32_t deviceCount = 0;

    VkResult res = vkEnumeratePhysicalDevices(
        instance.handle(),
        &deviceCount,
        nullptr);

    if (res != VK_SUCCESS || deviceCount == 0) {
        throw LSFG::vulkan_error(res, "No Vulkan devices found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);

    res = vkEnumeratePhysicalDevices(
        instance.handle(),
        &deviceCount,
        devices.data());

    if (res != VK_SUCCESS) {
        throw LSFG::vulkan_error(res, "Failed to enumerate devices");
    }

    /* -----------------------------------------------------
     * 2. Select physical device
     * ----------------------------------------------------- */
    std::optional<VkPhysicalDevice> physicalDevice;

    for (auto device : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);

        uint64_t uuid =
            (uint64_t(props.vendorID) << 32) | props.deviceID;

        std::cerr << "GPU vendor=" << props.vendorID
                  << " device=" << props.deviceID << std::endl;

        if (uuid == deviceUUID) {
            physicalDevice = device;
            std::cerr << "Matched target GPU" << std::endl;
            break;
        }
    }

    if (!physicalDevice && !devices.empty()) {
        std::cerr << "Fallback to first GPU" << std::endl;
        physicalDevice = devices[0];
    }

    if (!physicalDevice) {
        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "No valid GPU found");
    }

    /* -----------------------------------------------------
     * 3. Queue family selection
     * ----------------------------------------------------- */
    uint32_t familyCount = 0;

    vkGetPhysicalDeviceQueueFamilyProperties(
        *physicalDevice,
        &familyCount,
        nullptr);

    std::vector<VkQueueFamilyProperties> families(familyCount);

    vkGetPhysicalDeviceQueueFamilyProperties(
        *physicalDevice,
        &familyCount,
        families.data());

    std::optional<uint32_t> queueFamily;

    for (uint32_t i = 0; i < familyCount; i++) {
        auto flags = families[i].queueFlags;

        if ((flags & VK_QUEUE_GRAPHICS_BIT) &&
            (flags & VK_QUEUE_COMPUTE_BIT)) {
            queueFamily = i;
            break;
        }
    }

    if (!queueFamily) {
        for (uint32_t i = 0; i < familyCount; i++) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                queueFamily = i;
                break;
            }
        }
    }

    if (!queueFamily) {
        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "No compute queue family found");
    }

    std::cerr << "Selected queue family = " << *queueFamily << std::endl;

    /* -----------------------------------------------------
     * 4. Feature query
     * ----------------------------------------------------- */
    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
    };

    VkPhysicalDeviceFeatures2 features2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &features12
    };

    vkGetPhysicalDeviceFeatures2(*physicalDevice, &features2);

    bool fp16 =
        !forceDisableFp16 &&
        features12.shaderFloat16;

    this->supportsFP16 = fp16;

    std::cerr << (fp16 ?
        "FP16 enabled" :
        "FP32 fallback") << std::endl;

    /* -----------------------------------------------------
     * 5. External semaphore capability query (DEBUG ENHANCED)
     * ----------------------------------------------------- */
    VkPhysicalDeviceExternalSemaphoreInfo semInfo{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
    };

    VkExternalSemaphoreProperties semProps{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES
    };

    vkGetPhysicalDeviceExternalSemaphoreProperties(
        *physicalDevice,
        &semInfo,
        &semProps);

    std::cerr << "---- External Semaphore Capability ----" << std::endl;
    std::cerr << "exportable: "
              << ((semProps.externalSemaphoreFeatures &
                   VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) ? "YES" : "NO")
              << std::endl;

    std::cerr << "importable: "
              << ((semProps.externalSemaphoreFeatures &
                   VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) ? "YES" : "NO")
              << std::endl;

    std::cerr << "exportable fd type supported: "
              << ((semProps.exportFromImportedHandleTypes &
                   VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) ? "YES" : "NO")
              << std::endl;

    std::cerr << "importable fd type supported: "
              << ((semProps.compatibleHandleTypes &
                   VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) ? "YES" : "NO")
              << std::endl;

    bool supportsFdSemaphore =
        (semProps.externalSemaphoreFeatures &
         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) &&
        (semProps.externalSemaphoreFeatures &
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);

    /* -----------------------------------------------------
     * 6. Queue create
     * ----------------------------------------------------- */
    float priority = 1.0f;

    VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = *queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };

    /* -----------------------------------------------------
     * 7. Vulkan features chain
     * ----------------------------------------------------- */
    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE
    };

    features12.pNext = &features13;
    features12.timelineSemaphore = VK_TRUE;
    features12.vulkanMemoryModel = VK_TRUE;
    features12.shaderFloat16 = fp16;

    /* -----------------------------------------------------
     * 8. Device create
     * ----------------------------------------------------- */
    VkDeviceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features12,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount =
            static_cast<uint32_t>(requiredExtensions.size()),
        .ppEnabledExtensionNames = requiredExtensions.data()
    };

    VkDevice deviceHandle = VK_NULL_HANDLE;

    res = vkCreateDevice(
        *physicalDevice,
        &createInfo,
        nullptr,
        &deviceHandle);

    std::cerr << "vkCreateDevice = " << res << std::endl;

    if (res != VK_SUCCESS || deviceHandle == VK_NULL_HANDLE) {
        throw LSFG::vulkan_error(res, "Device creation failed");
    }

    volkLoadDevice(deviceHandle);

    VkQueue queue{};
    vkGetDeviceQueue(deviceHandle, *queueFamily, 0, &queue);

    this->computeQueue = queue;
    this->computeFamilyIdx = *queueFamily;
    this->physicalDevice = *physicalDevice;

    this->device = std::shared_ptr<VkDevice>(
        new VkDevice(deviceHandle),
        [](VkDevice* dev) {
            vkDestroyDevice(*dev, nullptr);
        }
    );
}
