#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>
#include <iostream>

using namespace Mini;

/*
 * Internal-only semaphore model
 * - No FD export/import
 * - No Win32 path
 * - No capability branching
 */

/* --------------------------------------------------------- */
/* Normal internal semaphore only                            */
/* --------------------------------------------------------- */
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
        throw LSFG::vulkan_error(
            res,
            "Failed to create internal semaphore");
    }

    this->semaphore =
        std::shared_ptr<VkSemaphore>(
            new VkSemaphore(handle),
            [dev = device](VkSemaphore* s) {
                Layer::ovkDestroySemaphore(dev, *s, nullptr);
            });
}

/* --------------------------------------------------------- */
/* External constructor removed entirely                     */
/* --------------------------------------------------------- */
/*
Semaphore::Semaphore(VkDevice device, int* fd)
{
    // REMOVED:
    // - external semaphore
    // - fd export/import
    // - fallback logic
}
*/
