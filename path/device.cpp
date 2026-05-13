#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/device.hpp"
#include "core/image.hpp"
#include "core/instance.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace LSFG::Core;

const std::vector<const char*> requiredExtensions = {
    // On Android we share via AHardwareBuffer, not opaque FDs.
    "VK_ANDROID_external_memory_android_hardware_buffer",
    "VK_KHR_external_memory",                  // base ext, dependency
    "VK_KHR_sampler_ycbcr_conversion",         // dependency of AHB ext
    "VK_KHR_dedicated_allocation",             // required for dedicated AHB import
    "VK_KHR_get_memory_requirements2",         // dependency
    "VK_KHR_bind_memory2",                     // dependency
    "VK_KHR_maintenance1",                     // dependency
};

namespace {

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}

// Compatibility shim for devices without VK_KHR_synchronization2 or Vulkan 1.3.
// Translates VkDependencyInfo (sync2 style) to the equivalent vkCmdPipelineBarrier
// call.  All stage/access flags the framegen uses are in the lower 32 bits and
// map 1:1 to their Vulkan 1.0 equivalents, so the static_cast is safe.
VKAPI_ATTR void VKAPI_CALL compat_pipeline_barrier2(
    VkCommandBuffer cmd, const VkDependencyInfo* dep)
{
    std::vector<VkImageMemoryBarrier> imgBarriers;
    VkPipelineStageFlags srcStages = 0, dstStages = 0;

    for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; ++i) {
        const auto& b = dep->pImageMemoryBarriers[i];
        srcStages |= static_cast<VkPipelineStageFlags>(b.srcStageMask);
        dstStages |= static_cast<VkPipelineStageFlags>(b.dstStageMask);
        imgBarriers.push_back(VkImageMemoryBarrier{
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask       = static_cast<VkAccessFlags>(b.srcAccessMask),
            .dstAccessMask       = static_cast<VkAccessFlags>(b.dstAccessMask),
            .oldLayout           = b.oldLayout,
            .newLayout           = b.newLayout,
            .srcQueueFamilyIndex = b.srcQueueFamilyIndex,
            .dstQueueFamilyIndex = b.dstQueueFamilyIndex,
            .image               = b.image,
            .subresourceRange    = b.subresourceRange,
        });
    }

    if (srcStages == 0) srcStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (dstStages == 0) dstStages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    vkCmdPipelineBarrier(cmd, srcStages, dstStages, dep->dependencyFlags,
                         0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(imgBarriers.size()), imgBarriers.data());
}

} // namespace

const Image& Device::getFallbackDescriptorImage() const {
    return *this->fallbackDescriptorImage;
}

Device::Device(const Instance& instance, uint64_t deviceUUID) {
    // get all physical devices
    uint32_t deviceCount{};
    auto res = vkEnumeratePhysicalDevices(instance.handle(), &deviceCount, nullptr);
    if (res != VK_SUCCESS || deviceCount == 0)
        throw LSFG::vulkan_error(res, "Failed to enumerate physical devices");

    std::vector<VkPhysicalDevice> devices(deviceCount);
    res = vkEnumeratePhysicalDevices(instance.handle(), &deviceCount, devices.data());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to get physical devices");

    // get device by uuid
    std::optional<VkPhysicalDevice> physicalDevice;
    VkPhysicalDeviceProperties selectedProps{};
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);

        const uint64_t uuid =
            static_cast<uint64_t>(properties.vendorID) << 32 | properties.deviceID;
        if (deviceUUID == uuid || deviceUUID == 0x1463ABAC) {
            physicalDevice = device;
            selectedProps = properties;
            break;
        }
    }
    if (!physicalDevice)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Could not find physical device with UUID");

    // find queue family indices
    uint32_t familyCount{};
    vkGetPhysicalDeviceQueueFamilyProperties(*physicalDevice, &familyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(*physicalDevice, &familyCount, queueFamilies.data());

    std::optional<uint32_t> computeFamilyIdx;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            computeFamilyIdx = i;
    }
    if (!computeFamilyIdx)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "No compute queue family found");

    uint32_t extensionCount{};
    res = vkEnumerateDeviceExtensionProperties(*physicalDevice, nullptr, &extensionCount, nullptr);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to enumerate device extensions");
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    res = vkEnumerateDeviceExtensionProperties(*physicalDevice, nullptr,
        &extensionCount, availableExtensions.data());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to get device extensions");

    std::vector<const char*> enabledExtensions;
    enabledExtensions.reserve(requiredExtensions.size() + 4);
    for (const char* extension : requiredExtensions) {
        if (!hasExtension(availableExtensions, extension)) {
            throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
                std::string("Missing required device extension: ") + extension);
        }
        enabledExtensions.push_back(extension);
    }

    // Determine which features are in core vs. need explicit extensions.
    // VkPhysicalDeviceVulkan12/13Features are only valid when the physical
    // device reports the matching API version; on older devices we use the
    // extension-specific structs (same sType values, aliased in 1.2/1.3).
    const bool api12 = selectedProps.apiVersion >= VK_API_VERSION_1_2;
    const bool api13 = selectedProps.apiVersion >= VK_API_VERSION_1_3;

    // Timeline semaphores: core in 1.2, extension on 1.1
    if (!api12) {
        if (!hasExtension(availableExtensions, "VK_KHR_timeline_semaphore"))
            throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
                "Missing required device extension: VK_KHR_timeline_semaphore");
        enabledExtensions.push_back("VK_KHR_timeline_semaphore");
    }

    // synchronization2 (vkCmdPipelineBarrier2): core in 1.3, extension on 1.1/1.2.
    // If neither is available we install compat_pipeline_barrier2 after device
    // creation to translate the new-style barriers to vkCmdPipelineBarrier.
    const bool hasSync2Ext = hasExtension(availableExtensions, "VK_KHR_synchronization2");
    if (!api13 && hasSync2Ext) {
        enabledExtensions.push_back("VK_KHR_synchronization2");
    }

    // vulkanMemoryModel: core in 1.2 but optional even there; needs extension on 1.1
    if (!api12 && hasExtension(availableExtensions, "VK_KHR_vulkan_memory_model"))
        enabledExtensions.push_back("VK_KHR_vulkan_memory_model");

    const bool hasRobustness2 =
        hasExtension(availableExtensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    if (hasRobustness2) {
        enabledExtensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    }

    // Query which optional features the device actually supports before requesting them.
    VkPhysicalDeviceVulkanMemoryModelFeaturesKHR memModelQuery{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR,
    };
    VkPhysicalDeviceFeatures2 features2Query{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &memModelQuery,
    };
    vkGetPhysicalDeviceFeatures2(*physicalDevice, &features2Query);
    const bool hasMemModel = memModelQuery.vulkanMemoryModel;

    // Query base features for storage-image capabilities used by DXVK-translated shaders.
    VkPhysicalDeviceFeatures baseFeatures{};
    vkGetPhysicalDeviceFeatures(*physicalDevice, &baseFeatures);

    // Build device feature chain using extension-specific structs.  Their sType
    // values are aliased to the promoted core structs on 1.2/1.3, so drivers
    // on all supported API versions will recognise them.
    const float queuePriority{1.0F};
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .nullDescriptor = VK_TRUE,
    };
    VkPhysicalDeviceVulkanMemoryModelFeaturesKHR memModel{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR,
        .pNext = hasRobustness2 ? &robustness2 : nullptr,
        .vulkanMemoryModel = hasMemModel ? VK_TRUE : VK_FALSE,
    };
    const bool enableSync2 = api13 || hasSync2Ext;
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
        .pNext = &memModel,
        .synchronization2 = VK_TRUE,
    };
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timelineSem{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
        .pNext = enableSync2 ? static_cast<void*>(&sync2) : static_cast<void*>(&memModel),
        .timelineSemaphore = VK_TRUE,
    };
    const VkDeviceQueueCreateInfo computeQueueDesc{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = *computeFamilyIdx,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority
    };
    // Enable storage-image-without-format features that DXVK-translated shaders require.
    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.shaderStorageImageReadWithoutFormat =
        baseFeatures.shaderStorageImageReadWithoutFormat;
    enabledFeatures.shaderStorageImageWriteWithoutFormat =
        baseFeatures.shaderStorageImageWriteWithoutFormat;

    const VkDeviceCreateInfo deviceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &timelineSem,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &computeQueueDesc,
        .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
        .ppEnabledExtensionNames = enabledExtensions.data(),
        .pEnabledFeatures = &enabledFeatures,
    };
    VkDevice deviceHandle{};
    res = vkCreateDevice(*physicalDevice, &deviceCreateInfo, nullptr, &deviceHandle);
    if (res != VK_SUCCESS || deviceHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create logical device");

    volkLoadDevice(deviceHandle);

    // If neither Vulkan 1.3 nor VK_KHR_synchronization2 is available, the
    // vkCmdPipelineBarrier2 pointer will be null after volkLoadDevice.
    // Install the compatibility shim so barrier calls work transparently.
    if (!vkCmdPipelineBarrier2)
        vkCmdPipelineBarrier2 = compat_pipeline_barrier2;

    // get compute queue
    VkQueue queueHandle{};
    vkGetDeviceQueue(deviceHandle, *computeFamilyIdx, 0, &queueHandle);

    // store in shared ptr
    this->computeQueue = queueHandle;
    this->computeFamilyIdx = *computeFamilyIdx;
    this->physicalDevice = *physicalDevice;
    this->nullDescriptorSupported = hasRobustness2;
    this->device = std::shared_ptr<VkDevice>(
        new VkDevice(deviceHandle),
        [](VkDevice* device) {
            vkDestroyDevice(*device, nullptr);
        }
    );
    if (!this->nullDescriptorSupported) {
        this->fallbackDescriptorImage = std::make_shared<Core::Image>(*this,
            VkExtent2D{1, 1}, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);

        // Descriptors bind this image with imageLayout=GENERAL; transition it
        // out of the UNDEFINED creation layout to avoid a validation hazard.
        VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = *computeFamilyIdx,
        };
        VkCommandPool initPool = VK_NULL_HANDLE;
        if (vkCreateCommandPool(deviceHandle, &poolInfo, nullptr, &initPool) == VK_SUCCESS) {
            const VkCommandBufferAllocateInfo cbai{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = initPool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            VkCommandBuffer initCb = VK_NULL_HANDLE;
            if (vkAllocateCommandBuffers(deviceHandle, &cbai, &initCb) == VK_SUCCESS) {
                const VkCommandBufferBeginInfo beginInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                };
                vkBeginCommandBuffer(initCb, &beginInfo);
                const VkImageMemoryBarrier toGeneral{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = this->fallbackDescriptorImage->handle(),
                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                };
                vkCmdPipelineBarrier(initCb,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &toGeneral);
                vkEndCommandBuffer(initCb);
                const VkSubmitInfo si{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &initCb,
                };
                vkQueueSubmit(queueHandle, 1, &si, VK_NULL_HANDLE);
                vkQueueWaitIdle(queueHandle);
            }
            vkDestroyCommandPool(deviceHandle, initPool, nullptr);
        }
    }
}
