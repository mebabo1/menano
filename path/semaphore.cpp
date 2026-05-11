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

    auto res = Layer::ovkCreateSemaphore(
        device,
        &desc,
        nullptr,
        &semaphoreHandle
    );

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
    // Termux glibc / Turnip fallback:
    // disable external semaphore fd export completely

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

    // no external fd export
    if (fd)
        *fd = -1;

    // store semaphore in shared ptr
    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device](VkSemaphore* semaphoreHandle) {
            Layer::ovkDestroySemaphore(dev, *semaphoreHandle, nullptr);
        }
    );
}
