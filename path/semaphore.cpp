#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <iostream>

using namespace Mini;

/* ---------------------------------------------------------
 * Capability ignored (kept for compatibility only)
 * --------------------------------------------------------- */
void Mini::setSemaphoreCapabilities(bool, bool) {
    // no-op in internal-only mode
}

/* ---------------------------------------------------------
 * Internal semaphore
 * --------------------------------------------------------- */
Semaphore::Semaphore(VkDevice device) {

    VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };

    VkSemaphore handle = VK_NULL_HANDLE;

    VkResult res = Layer::ovkCreateSemaphore(
        device,
        &desc,
        nullptr,
        &handle);

    if (res != VK_SUCCESS || handle == VK_NULL_HANDLE) {
        throw LSFG::vulkan_error(res, "Failed to create semaphore");
    }

    semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(handle),
        [dev = device](VkSemaphore* s) {
            Layer::ovkDestroySemaphore(dev, *s, nullptr);
        }
    );
}

/* ---------------------------------------------------------
 * External constructor kept for ABI, but DISABLED
 * --------------------------------------------------------- */
Semaphore::Semaphore(VkDevice device, int* fd) {

    (void)fd;

    VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };

    VkSemaphore handle = VK_NULL_HANDLE;

    VkResult res = Layer::ovkCreateSemaphore(
        device,
        &desc,
        nullptr,
        &handle);

    if (res != VK_SUCCESS || handle == VK_NULL_HANDLE) {
        throw LSFG::vulkan_error(res, "Failed to create internal semaphore");
    }

    if (fd) {
        *fd = -1;
    }

    std::cerr << "Internal-only framegen mode enabled" << std::endl;

    semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(handle),
        [dev = device](VkSemaphore* s) {
            Layer::ovkDestroySemaphore(dev, *s, nullptr);
        }
    );
}
