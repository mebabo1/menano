#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>
#include <memory>
#include <cstdio> // fprintf 사용을 위해 추가

using namespace Mini;

Semaphore::Semaphore(VkDevice device) {
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device](VkSemaphore* h) {
            if (h) {
                Layer::ovkDestroySemaphore(dev, *h, nullptr);
                delete h;
            }
        }
    );
}

Semaphore::Semaphore(VkDevice device, int* fd) {
    // 1. 안드로이드 드라이버와 호환성이 좋은 SYNC_FD 타입으로 생성 시도
    const VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
    };
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exportInfo
    };
    
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    
    // 2. Export용 세마포어 생성 실패 시 일반 세마포어로 폴백
    if (res != VK_SUCCESS) {
        const VkSemaphoreCreateInfo fallbackDesc{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        Layer::ovkCreateSemaphore(device, &fallbackDesc, nullptr, &semaphoreHandle);
        if (fd) *fd = -1; 
    } else {
        // 3. SYNC_FD 추출 시도
        const VkSemaphoreGetFdInfoKHR fdInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = semaphoreHandle,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
        };
        res = Layer::ovkGetSemaphoreFdKHR(device, &fdInfo, fd);
        
        if (res != VK_SUCCESS) {
            // [핵심 수정] 추출 실패 시 GPU를 강제로 대기시켜 이미지 준비를 보장합니다.
            // 이 처리가 있어야 FD가 -1이라도 프레임 생성기가 정상 데이터를 가져갑니다.
            Layer::ovkDeviceWaitIdle(device);
            if (fd) *fd = -1;
        }
    }

    // 4. 스마트 포인터에 저장하여 메모리 관리
    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device](VkSemaphore* h) {
            if (h) {
                Layer::ovkDestroySemaphore(dev, *h, nullptr);
                delete h;
            }
        }
    );
}
