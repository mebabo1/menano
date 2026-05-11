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

// Minimal extension set for Turnip/Termux-glibc
const std::vector<const char*> requiredExtensions = {
    "VK_KHR_external_memory_fd",
    "VK_KHR_external_semaphore_fd"
};

Device::Device(const Instance& instance, uint64_t deviceUUID, bool forceDisableFp16) {
    // enumerate physical devices
    uint32_t deviceCount{};
    auto res = vkEnumeratePhysicalDevices(instance.handle(), &deviceCount, nullptr);

    if (res != VK_SUCCESS || deviceCount == 0)
        throw LSFG::vulkan_error(res, "Failed to enumerate physical devices");

    std::vector<VkPhysicalDevice> devices(deviceCount);

    res = vkEnumeratePhysicalDevices(
        instance.handle(),
        &deviceCount,
        devices.data());

    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to get physical devices");

    // select device
    std::optional<VkPhysicalDevice> physicalDevice;

    for (const auto& device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);

        const uint64_t uuid =
            (static_cast<uint64_t>(properties.vendorID) << 32)
            | properties.deviceID;

        if (deviceUUID == uuid || deviceUUID == 0x1463ABAC) {
            physicalDevice = device;
            break;
        }
    }

    if (!physicalDevice)
        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "Could not find physical device with UUID");

    // queue family
    uint32_t familyCount{};
    vkGetPhysicalDeviceQueueFamilyProperties(
        *physicalDevice,
        &familyCount,
        nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(familyCount);

    vkGetPhysicalDeviceQueueFamilyProperties(
        *physicalDevice,
        &familyCount,
        queueFamilies.data());

    std::optional<uint32_t> computeFamilyIdx;

    for (uint32_t i = 0; i < familyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            computeFamilyIdx = i;
            break;
        }
    }

    if (!computeFamilyIdx)
        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "No compute queue family found");

    // probe FP16 only
    VkPhysicalDeviceVulkan12Features supported12Features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
    };

    VkPhysicalDeviceFeatures2 supportedFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &supported12Features
    };

    vkGetPhysicalDeviceFeatures2(
        *physicalDevice,
        &supportedFeatures);

    this->supportsFP16 =
        !forceDisableFp16 &&
        supported12Features.shaderFloat16;

    if (this->supportsFP16)
        std::cerr << "lsfg-vk: Using FP16 acceleration\n";
    else
        std::cerr << "lsfg-vk: Using FP32 path\n";

    // queue info
    const float queuePriority = 1.0f;

    const VkDeviceQueueCreateInfo computeQueueDesc{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = *computeFamilyIdx,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority
    };

    // ONLY request shaderFloat16
    // disable timeline/sync2/vulkanMemoryModel
    const VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .shaderFloat16 = this->supportsFP16,
        .timelineSemaphore = VK_FALSE,
        .vulkanMemoryModel = VK_FALSE
    };

    const VkDeviceCreateInfo deviceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features12,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &computeQueueDesc,
        .enabledExtensionCount =
            static_cast<uint32_t>(requiredExtensions.size()),
        .ppEnabledExtensionNames =
            requiredExtensions.data()
    };

    VkDevice deviceHandle{};

    res = vkCreateDevice(
        *physicalDevice,
        &deviceCreateInfo,
        nullptr,
        &deviceHandle);

    if (res != VK_SUCCESS || deviceHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(
            res,
            "Failed to create logical device");

    volkLoadDevice(deviceHandle);

    // get queue
    VkQueue queueHandle{};
    vkGetDeviceQueue(
        deviceHandle,
        *computeFamilyIdx,
        0,
        &queueHandle);

    // store
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
