#include "mini/image.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>
#include <memory>

using namespace Mini;

Image::Image(VkDevice device, VkPhysicalDevice physicalDevice,
        VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags,
        int* fd) 
        : extent(extent), format(format), aspectFlags(aspectFlags) {

    // 안드로이드 관련 AHB 할당 코드를 모두 제거하고 표준 VkImage 생성
    VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage imageHandle{};
    auto res = Layer::ovkCreateImage(device, &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS) throw LSFG::vulkan_error(res, "Failed to create image");

    // 표준 메모리 할당 로직 (vkGetImageMemoryRequirements 등 사용)
    VkMemoryRequirements memReqs;
    Layer::ovkGetImageMemoryRequirements(device, imageHandle, &memReqs);

    VkPhysicalDeviceMemoryProperties memProps;
    Layer::ovkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    uint32_t typeIndex = 0; // 적절한 메모리 타입 찾는 로직 (기존 유지)
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1u << i)) && 
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            typeIndex = i; break;
        }
    }

    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = typeIndex,
    };

    VkDeviceMemory memoryHandle{};
    Layer::ovkAllocateMemory(device, &allocInfo, nullptr, &memoryHandle);
    Layer::ovkBindImageMemory(device, imageHandle, memoryHandle, 0);

    // FD는 glibc에서 의미가 없으므로 무시
    if (fd) *fd = -1;

    // 객체 저장
    this->image = std::shared_ptr<VkImage>(new VkImage(imageHandle), [dev=device](auto h){ Layer::ovkDestroyImage(dev, *h, nullptr); delete h; });
    this->memory = std::shared_ptr<VkDeviceMemory>(new VkDeviceMemory(memoryHandle), [dev=device](auto h){ Layer::ovkFreeMemory(dev, *h, nullptr); delete h; });
        }
