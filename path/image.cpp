#include "mini/image.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>
#include <memory>
#include <cstdint>
#include <optional>
#include <iostream>

using namespace Mini;

Image::Image(VkDevice device, VkPhysicalDevice physicalDevice,
        VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags, int* fd)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    
    // 1. 외부 메모리 인포 (glibc에서는 OPAQUE_FD 사용)
    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
    };

    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &externalInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { .width = extent.width, .height = extent.height, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, // 추가
        .usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage imageHandle{};
    auto res = Layer::ovkCreateImage(device, &desc, nullptr, &imageHandle);
    
    // 폴백: 외부 메모리 설정 없이 재시도 (FD 추출은 포기하더라도 객체 생성은 성공시켜야 함)
    if (res != VK_SUCCESS) {
        VkImageCreateInfo fallbackDesc = desc;
        fallbackDesc.pNext = nullptr;
        res = Layer::ovkCreateImage(device, &fallbackDesc, nullptr, &imageHandle);
    }

    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image");

    // 2. 메모리 요구사항 확인 및 타입 선택
    VkMemoryRequirements memReqs;
    Layer::ovkGetImageMemoryRequirements(device, imageHandle, &memReqs);

    VkPhysicalDeviceMemoryProperties memProps;
    Layer::ovkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    uint32_t memTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) && 
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memTypeIndex = i;
            break;
        }
    }

    if (memTypeIndex == UINT32_MAX) { // 폴백 로직
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if (memReqs.memoryTypeBits & (1 << i)) {
                memTypeIndex = i; break;
            }
        }
    }

    // 3. 메모리 할당 (Export 설정 포함)
    VkExportMemoryAllocateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
    };

    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &exportInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memTypeIndex
    };

    VkDeviceMemory memoryHandle{};
    res = Layer::ovkAllocateMemory(device, &allocInfo, nullptr, &memoryHandle);
    
    // 폴백: Export 없이 할당
    if (res != VK_SUCCESS) {
        allocInfo.pNext = nullptr;
        res = Layer::ovkAllocateMemory(device, &allocInfo, nullptr, &memoryHandle);
    }

    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to allocate memory");

    Layer::ovkBindImageMemory(device, imageHandle, memoryHandle, 0);

    // 4. FD 추출 (실패해도 throw 하지 않음)
    if (fd) {
        *fd = -1; // 초기값
        VkMemoryGetFdInfoKHR fdGetInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = memoryHandle,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
        };
        
        // 실제 추출 시도
        res = Layer::ovkGetMemoryFdKHR(device, &fdGetInfo, fd);
        
        if (res != VK_SUCCESS) {
            // std::cerr << "lsfg-vk: Warning: Could not export memory FD" << std::endl;
            *fd = -1; 
        }
    }

    // 5. 스마트 포인터 저장
    this->image = std::shared_ptr<VkImage>(new VkImage(imageHandle), [dev=device](VkImage* img) {
        if (img) { Layer::ovkDestroyImage(dev, *img, nullptr); delete img; }
    });
    this->memory = std::shared_ptr<VkDeviceMemory>(new VkDeviceMemory(memoryHandle), [dev=device](VkDeviceMemory* mem) {
        if (mem) { Layer::ovkFreeMemory(dev, *mem, nullptr); delete mem; }
    });
        }
