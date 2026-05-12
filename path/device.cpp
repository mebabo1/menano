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
#include <string>

using namespace LSFG::Core;

/* ---------------------------------------------------------
 * Extension query helpers
 * --------------------------------------------------------- */
static std::vector<std::string> queryDeviceExtensions(VkPhysicalDevice device)
{
    uint32_t count = 0;

    vkEnumerateDeviceExtensionProperties(
        device,
        nullptr,
        &count,
        nullptr
    );

    std::vector<VkExtensionProperties> props(count);

    vkEnumerateDeviceExtensionProperties(
        device,
        nullptr,
        &count,
        props.data()
    );

    std::vector<std::string> out;
    out.reserve(count);

    for (const auto& p : props)
        out.emplace_back(p.extensionName);

    return out;
}

static bool hasExt(
    const std::vector<std::string>& list,
    const char* name)
{
    for (const auto& e : list)
        if (e == name)
            return true;
    return false;
}

/* ---------------------------------------------------------
 * Device constructor
 * --------------------------------------------------------- */
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

    if (res != VK_SUCCESS || deviceCount == 0)
        throw LSFG::vulkan_error(res, "No Vulkan devices found");

    std::vector<VkPhysicalDevice> devices(deviceCount);

    res = vkEnumeratePhysicalDevices(
        instance.handle(),
        &deviceCount,
        devices.data());

    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to enumerate devices");

    /* -----------------------------------------------------
     * 2. Select physical device
     * ----------------------------------------------------- */
    std::optional<VkPhysicalDevice> physicalDevice;

    for (auto device : devices)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);

        uint64_t uuid =
            (uint64_t(props.vendorID) << 32) | props.deviceID;

        std::cerr
            << "GPU vendor=" << props.vendorID
            << " device=" << props.deviceID
            << std::endl;

        if (uuid == deviceUUID)
        {
            physicalDevice = device;
            std::cerr << "Matched target GPU" << std::endl;
            break;
        }
    }

    if (!physicalDevice && !devices.empty())
    {
        std::cerr << "Fallback to first GPU" << std::endl;
        physicalDevice = devices[0];
    }

    if (!physicalDevice)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "No valid GPU found");

    /* -----------------------------------------------------
     * 3. Query available extensions
     * ----------------------------------------------------- */
    auto availableExt = queryDeviceExtensions(*physicalDevice);

    std::vector<const char*> enabledExtensions;

    enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    bool supportsExternalSemaphore =
        hasExt(availableExt, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) &&
        hasExt(availableExt, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

    if (supportsExternalSemaphore)
    {
        enabledExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
        enabledExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    }
    else
    {
        std::cerr << "External semaphore NOT supported → internal-only mode\n";
    }

    bool supportsExternalMemory =
        hasExt(availableExt, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) &&
        hasExt(availableExt, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);

    if (supportsExternalMemory)
    {
        enabledExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        enabledExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    }

    if (hasExt(availableExt, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME))
        enabledExtensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

    /* -----------------------------------------------------
     * 4. Queue family selection
     * ----------------------------------------------------- */
    uint32_t familyCount = 0;

    vkGetPhysicalDeviceQueueFamilyProperties(
        *physicalDevice, &familyCount, nullptr);

    std::vector<VkQueueFamilyProperties> families(familyCount);

    vkGetPhysicalDeviceQueueFamilyProperties(
        *physicalDevice, &familyCount, families.data());

    std::optional<uint32_t> queueFamily;

    for (uint32_t i = 0; i < familyCount; i++)
    {
        auto flags = families[i].queueFlags;

        if ((flags & VK_QUEUE_GRAPHICS_BIT) &&
            (flags & VK_QUEUE_COMPUTE_BIT))
        {
            queueFamily = i;
            break;
        }
    }

    if (!queueFamily)
    {
        for (uint32_t i = 0; i < familyCount; i++)
        {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            {
                queueFamily = i;
                break;
            }
        }
    }

    if (!queueFamily)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "No compute queue family found");

    std::cerr << "Selected queue family = " << *queueFamily << std::endl;

    /* -----------------------------------------------------
     * 5. Feature query
     * ----------------------------------------------------- */
    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
    };

    VkPhysicalDeviceFeatures2 features2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &features12
    };

    vkGetPhysicalDeviceFeatures2(*physicalDevice, &features2);

    bool fp16 = !forceDisableFp16 && features12.shaderFloat16;

    this->supportsFP16 = fp16;

    std::cerr << (fp16 ? "FP16 enabled" : "FP32 fallback") << std::endl;

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
     * 7. Vulkan 1.3 features
     * ----------------------------------------------------- */
    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE
    };

    /* -----------------------------------------------------
     * 8. Vulkan 1.2 features
     * ----------------------------------------------------- */
    features12.pNext = &features13;
    features12.shaderFloat16 = fp16;
    features12.timelineSemaphore = VK_TRUE;
    features12.vulkanMemoryModel = VK_TRUE;

    /* -----------------------------------------------------
     * 9. Device create
     * ----------------------------------------------------- */
    VkDeviceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features12,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount =
            static_cast<uint32_t>(enabledExtensions.size()),
        .ppEnabledExtensionNames =
            enabledExtensions.data()
    };

    VkDevice deviceHandle = VK_NULL_HANDLE;

    res = vkCreateDevice(
        *physicalDevice,
        &createInfo,
        nullptr,
        &deviceHandle);

    std::cerr << "vkCreateDevice = " << res << std::endl;

    if (res != VK_SUCCESS || deviceHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create device");

    volkLoadDevice(deviceHandle);

    /* -----------------------------------------------------
     * 10. Queue handle
     * ----------------------------------------------------- */
    VkQueue queueHandle{};
    vkGetDeviceQueue(deviceHandle, *queueFamily, 0, &queueHandle);

    this->computeQueue = queueHandle;
    this->computeFamilyIdx = *queueFamily;
    this->physicalDevice = *physicalDevice;

    this->device = std::shared_ptr<VkDevice>(
        new VkDevice(deviceHandle),
        [](VkDevice* d) {
            vkDestroyDevice(*d, nullptr);
        }
    );
}
