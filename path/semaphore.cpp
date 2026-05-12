#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>
#include <memory>
#include <iostream>

using namespace Mini;

/*
 * Internal-only semaphore model
 * - No external semaphore
 * - No FD / Win32 export
 * - Vulkan-only sync
 */

Semaphore::Semaphore(VkDevice device)
{
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkSemaphore handle = VK_NULL_HANDLE;

    VkResult res = Layer::ovkCreateSemaphore(
        device,
        &info,
        nullptr,
        &handle);

    if (res != VK_SUCCESS || handle == VK_NULL_HANDLE) {
        throw LSFG::vulkan_error(
            res,
            "Failed to create internal semaphore");
    }

    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(handle),
        [dev = device](VkSemaphore* s) {
            Layer::ovkDestroySemaphore(dev, *s, nullptr);
        }
    );

    std::cerr << "lsfg-vk: internal semaphore created" << std::endl;
}
