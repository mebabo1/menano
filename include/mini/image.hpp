#pragma once

#include <vulkan/vulkan_core.h>
#include <memory>

// Android Hardware Buffer 타입을 인식하기 위한 선언
struct AHardwareBuffer;

namespace Mini {

    class Image {
    public:
        Image() noexcept = default;

        // .cpp 파일과 일치하도록 파라미터가 선언되어 있습니다.
        Image(VkDevice device, 
              VkPhysicalDevice physicalDevice, 
              VkExtent2D extent, 
              VkFormat format,
              VkImageUsageFlags usage, 
              VkImageAspectFlags aspectFlags,
              int* fd = nullptr);

        // AHardwareBuffer*를 반환하기 위해 이 함수가 필요합니다.
        [[nodiscard]] AHardwareBuffer* getAhb() const { return this->ahb; }

        [[nodiscard]] auto handle() const { return *this->image; }
        [[nodiscard]] auto getMemory() const { return *this->memory; }
        [[nodiscard]] VkExtent2D getExtent() const { return this->extent; }
        [[nodiscard]] VkFormat getFormat() const { return this->format; }
        [[nodiscard]] VkImageAspectFlags getAspectFlags() const { return this->aspectFlags; }

        Image(const Image&) noexcept = default;
        Image& operator=(const Image&) noexcept = default;
        Image(Image&&) noexcept = default;
        Image& operator=(Image&&) noexcept = default;
        ~Image() = default;

    private:
        std::shared_ptr<VkImage> image;
        std::shared_ptr<VkDeviceMemory> memory;

        AHardwareBuffer* ahb{nullptr}; 
        std::shared_ptr<AHardwareBuffer> ahbRef; 

        VkExtent2D extent{};
        VkFormat format{};
        VkImageAspectFlags aspectFlags{};
    };
}
