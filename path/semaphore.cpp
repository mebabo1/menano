#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>

using namespace Mini;

Semaphore::Semaphore(VkDevice device) {
    // create semaphore
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    // store semaphore in shared ptr
    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device](VkSemaphore* semaphoreHandle) {
            Layer::ovkDestroySemaphore(dev, *semaphoreHandle, nullptr);
        }
    );
}

Semaphore::Semaphore(VkDevice device, int* fd) {
    // 1. SYNC_FD 타입으로 세마포어 생성 시도
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
    
    // 2. 실패 시 일반 세마포어로 폴백
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
        
        if (res != VK_SUCCESS && fd) *fd = -1;
    }

    // 4. 스마트 포인터에 저장 (기존 (...) 부분을 이 코드로 교체)
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
