#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>
#include <iostream>

using namespace Mini;

Semaphore::Semaphore(VkDevice device) {

    /*
     * Normal Vulkan semaphore
     */

    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };

    VkSemaphore semaphoreHandle{};

    auto res =
        Layer::ovkCreateSemaphore(
            device,
            &desc,
            nullptr,
            &semaphoreHandle);

    if (res != VK_SUCCESS ||
        semaphoreHandle == VK_NULL_HANDLE) {

        throw LSFG::vulkan_error(
            res,
            "Unable to create semaphore");
    }

    /*
     * Store semaphore
     */

    this->semaphore =
        std::shared_ptr<VkSemaphore>(
            new VkSemaphore(semaphoreHandle),
            [dev = device](
                VkSemaphore* semaphoreHandle) {

                Layer::ovkDestroySemaphore(
                    dev,
                    *semaphoreHandle,
                    nullptr);
            });
}

Semaphore::Semaphore(VkDevice device, int* fd) {

    /*
     * Try exportable semaphore first.
     *
     * Turnip/Termux may support this partially.
     */

    const VkExportSemaphoreCreateInfo exportInfo{
        .sType =
            VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes =
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
    };

    const VkSemaphoreCreateInfo desc{
        .sType =
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exportInfo
    };

    VkSemaphore semaphoreHandle{};

    auto res =
        Layer::ovkCreateSemaphore(
            device,
            &desc,
            nullptr,
            &semaphoreHandle);

    /*
     * Fallback:
     * create normal semaphore if exportable
     * semaphore creation fails.
     */

    if (res != VK_SUCCESS ||
        semaphoreHandle == VK_NULL_HANDLE) {

        std::cerr
            << "lsfg-vk: external semaphore unsupported, using fallback"
            << std::endl;

        const VkSemaphoreCreateInfo fallbackDesc{
            .sType =
                VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
        };

        res =
            Layer::ovkCreateSemaphore(
                device,
                &fallbackDesc,
                nullptr,
                &semaphoreHandle);

        if (res != VK_SUCCESS ||
            semaphoreHandle == VK_NULL_HANDLE) {

            throw LSFG::vulkan_error(
                res,
                "Unable to create semaphore");
        }

        if (fd)
            *fd = -1;
    }
    else {

        /*
         * Export semaphore FD
         */

        const VkSemaphoreGetFdInfoKHR fdInfo{
            .sType =
                VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = semaphoreHandle,
            .handleType =
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
        };

        res =
            Layer::ovkGetSemaphoreFdKHR(
                device,
                &fdInfo,
                fd);

        /*
         * Export failed.
         *
         * Keep semaphore alive anyway.
         */

        if (res != VK_SUCCESS ||
            !fd ||
            *fd < 0) {

            std::cerr
                << "lsfg-vk: semaphore fd export failed, fallback mode"
                << std::endl;

            if (fd)
                *fd = -1;
        }
    }

    /*
     * Store semaphore
     */

    this->semaphore =
        std::shared_ptr<VkSemaphore>(
            new VkSemaphore(semaphoreHandle),
            [dev = device](
                VkSemaphore* semaphoreHandle) {

                Layer::ovkDestroySemaphore(
                    dev,
                    *semaphoreHandle,
                    nullptr);
            });
}
