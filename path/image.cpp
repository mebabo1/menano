#include "mini/image.hpp"
#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>
#include <memory>
#include <cstdint>
#include <dlfcn.h> // 동적 로딩을 위해 필요

// --- [glibc 우회: 안드로이드 AHB 인터페이스 직접 정의] ---
extern "C" {
    // 1. AHB 관련 상수 정의
    enum {
        AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1,
        AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT = 0x16,
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE = 1u << 8,
        AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT = 1u << 9,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN = 3u << 2
    };

    // 2. AHB 구조체 선언
    typedef struct AHardwareBuffer_Desc {
        uint32_t width;
        uint32_t height;
        uint32_t layers;
        uint32_t format;
        uint64_t usage;
        uint32_t stride;
        uint32_t rfu0;
        uint64_t rfu1;
    } AHardwareBuffer_Desc;

    struct AHardwareBuffer;

    // 3. Vulkan 안드로이드 확장 구조체 직접 선언 (vulkan_android.h 우회)
    typedef struct VkImportAndroidHardwareBufferInfoANDROID {
        VkStructureType sType;
        const void* pNext;
        struct AHardwareBuffer* buffer;
    } VkImportAndroidHardwareBufferInfoANDROID;

    #define VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID (VkStructureType)1000129000
    #define VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID (VkExternalMemoryHandleTypeFlagBits)0x00000400

    // 4. 안드로이드 시스템 함수 프로토타입 (런타임에 libandroid.so에서 찾아씀)
    int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, struct AHardwareBuffer** outBuffer);
    void AHardwareBuffer_release(struct AHardwareBuffer* buffer);
}
// -------------------------------------------------------

using namespace Mini;

Image::Image(VkDevice device, VkPhysicalDevice physicalDevice,
        VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags,
        int* fd) 
        : extent(extent), format(format), aspectFlags(aspectFlags) {

    // [이하 기존 로직 동일]
    if (fd) {
        try {
            auto syncSemaphore = std::make_unique<Semaphore>(device, fd);
        } catch (...) {
            if (fd) *fd = -1;
        }
    }

    // 2. Format 변환 로직
    uint32_t ahbFormat = 0;
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:        ahbFormat = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:   ahbFormat = AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT; break;
        default:
            throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
                "Unsupported VkFormat for AHB allocation");
    }

    // 3. Allocate AHardwareBuffer
    AHardwareBuffer_Desc ahbDesc{
        .width = extent.width,
        .height = extent.height,
        .layers = 1,
        .format = ahbFormat,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
               | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
               | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        .stride = 0,
        .rfu0 = 0,
        .rfu1 = 0,
    };
    AHardwareBuffer* ahbHandle{};
    if (AHardwareBuffer_allocate(&ahbDesc, &ahbHandle) != 0 || ahbHandle == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "Failed to allocate AHardwareBuffer for image");
    this->ahb = ahbHandle;

    // 4. Create VkImage wrapping the AHB
    VkExternalMemoryImageCreateInfo extImageInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &extImageInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT
               | VK_IMAGE_USAGE_STORAGE_BIT
               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
               | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage imageHandle{};
    auto res = Layer::ovkCreateImage(device, &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image from AHB");

    // 5. Get memory requirements
    VkMemoryRequirements memReqs;
    Layer::ovkGetImageMemoryRequirements(device, imageHandle, &memReqs);

    // 6. Find memory type
    VkPhysicalDeviceMemoryProperties memProps;
    Layer::ovkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    uint32_t typeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == UINT32_MAX) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if (memReqs.memoryTypeBits & (1u << i)) {
                typeIndex = i;
                break;
            }
        }
    }
    if (typeIndex == UINT32_MAX)
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "No memory type matches AHB image requirements");

    // 7. Import AHB into Vulkan memory
    VkMemoryDedicatedAllocateInfo dedicatedInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = imageHandle,
    };
    VkImportAndroidHardwareBufferInfoANDROID importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext = &dedicatedInfo,
        .buffer = ahbHandle,
    };
    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &importInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = typeIndex,
    };
    VkDeviceMemory memoryHandle{};
    res = Layer::ovkAllocateMemory(device, &allocInfo, nullptr, &memoryHandle);
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to import AHB into Vulkan memory");

    res = Layer::ovkBindImageMemory(device, imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind AHB memory to Vulkan image");

    // 8. Store objects with proper cleanup
    this->image = std::shared_ptr<VkImage>(
        new VkImage(imageHandle),
        [dev = device](VkImage* img) {
            Layer::ovkDestroyImage(dev, *img, nullptr);
            delete img;
        }
    );
    this->memory = std::shared_ptr<VkDeviceMemory>(
        new VkDeviceMemory(memoryHandle),
        [dev = device](VkDeviceMemory* mem) {
            Layer::ovkFreeMemory(dev, *mem, nullptr);
            delete mem;
        }
    );
    this->ahbRef = std::shared_ptr<AHardwareBuffer>(
        ahbHandle,
        [](AHardwareBuffer* b) {
            if (b) AHardwareBuffer_release(b);
        }
    );

} // 생성자 끝 (이 닫는 중괄호가 마지막에 있어야 합니다)
