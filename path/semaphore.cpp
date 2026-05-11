#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>

using namespace Mini;

Semaphore::Semaphore(VkDevice device) {
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };

    VkSemaphore semaphoreHandle{};

    auto res = Layer::ovkCreateSemaphore(
        device,
        &desc,
        nullptr,
        &semaphoreHandle
    );

    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device](VkSemaphore* semaphoreHandle) {
            Layer::ovkDestroySemaphore(dev, *semaphoreHandle, nullptr);
        }
    );
}

Semaphore::Semaphore(VkDevice device, int* fd) {

    // Linux/Turnip friendly semaphore export
    const VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,

        // CHANGE THIS
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
    };

    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exportInfo
    };

    VkSemaphore semaphoreHandle{};

    auto res = Layer::ovkCreateSemaphore(
        device,
        &desc,
        nullptr,
        &semaphoreHandle
    );

    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    const VkSemaphoreGetFdInfoKHR fdInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = semaphoreHandle,

        // CHANGE THIS TOO
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
    };

    res = Layer::ovkGetSemaphoreFdKHR(device, &fdInfo, fd);

    if (res != VK_SUCCESS || *fd < 0)
        throw LSFG::vulkan_error(res, "Unable to export semaphore to fd");

    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device](VkSemaphore* semaphoreHandle) {
            Layer::ovkDestroySemaphore(dev, *semaphoreHandle, nullptr);
        }
    );
}
