#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>
#include <memory>
#include <cstdio>

using namespace Mini;

Semaphore::Semaphore(VkDevice device) {
    // 일반 세마포어 생성 (내부용)
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    // 스마트 포인터 관리 및 소멸자 등록
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
    // 1. SYNC_FD 타입으로 세마포어 수출(Export) 설정
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
    
    // 2. 수출용 세마포어 생성 실패 시 일반 세마포어로 폴백
    if (res != VK_SUCCESS) {
        const VkSemaphoreCreateInfo fallbackDesc{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        Layer::ovkCreateSemaphore(device, &fallbackDesc, nullptr, &semaphoreHandle);
        if (fd) *fd = -1; 
    } else {
        // 3. 실제 FD(파일 디스크립터) 추출 시도
        const VkSemaphoreGetFdInfoKHR fdInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = semaphoreHandle,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
        };
        res = Layer::ovkGetSemaphoreFdKHR(device, &fdInfo, fd);
        
        if (res != VK_SUCCESS) {
            // [중요] 추출 실패 시(FD: -1) GPU를 강제로 대기시켜 데이터 정합성을 확보합니다.
            // lsfg-vk의 Layer 클래스 명명 규칙에 따라 ovkWaitDeviceIdle을 사용합니다.
            Layer::ovkWaitDeviceIdle(device);
            if (fd) *fd = -1;
        }
    }

    // 4. 세마포어 핸들을 shared_ptr로 래핑하여 안전하게 관리
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
