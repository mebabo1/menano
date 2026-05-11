#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>
#include <memory>

using namespace Mini;

// 기본 생성자
Semaphore::Semaphore(VkDevice device) {
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to create basic semaphore");

    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device](VkSemaphore* h) {
            if (h && *h != VK_NULL_HANDLE) {
                Layer::ovkDestroySemaphore(dev, *h, nullptr);
            }
            delete h;
        }
    );
}

// FD를 인자로 받는 생성자 (glibc 우회 버전)
Semaphore::Semaphore(VkDevice device, int* fd) {
    // 안드로이드 전용 SYNC_FD 확장을 완전히 제거합니다.
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr
    };
    
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to create semaphore");

    // glibc 환경에서는 FD 추출이 불가능하므로 항상 -1로 설정합니다.
    // 대신 프레임 생성기는 이 세마포어 핸들 자체를 Vulkan API 내에서 사용하게 됩니다.
    if (fd) *fd = -1;

    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device](VkSemaphore* h) {
            if (h && *h != VK_NULL_HANDLE) {
                Layer::ovkDestroySemaphore(dev, *h, nullptr);
            }
            delete h;
        }
    );
}
