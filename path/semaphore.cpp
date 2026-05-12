#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>
#include <iostream>

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

    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
        throw LSFG::vulkan_error(res, "Unable to create semaphore");
    }

    this->semaphore =
        std::shared_ptr<VkSemaphore>(
            new VkSemaphore(semaphoreHandle),
            [dev = device](VkSemaphore* semaphoreHandle) {
                Layer::ovkDestroySemaphore(
                    dev,
                    *semaphoreHandle,
                    nullptr
                );
            }
        );
}

Semaphore::Semaphore(VkDevice device, int* fd) {

    bool externalSupported = true;

    const VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
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

    /*
     * =========================================================
     * CASE 1: External semaphore supported (ideal path)
     * =========================================================
     */
    if (res == VK_SUCCESS && semaphoreHandle != VK_NULL_HANDLE) {

        const VkSemaphoreGetFdInfoKHR fdInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = semaphoreHandle,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
        };

        res = Layer::ovkGetSemaphoreFdKHR(device, &fdInfo, fd);

        if (res != VK_SUCCESS || !fd || *fd < 0) {

            std::cerr
                << "lsfg-vk: external fd export failed -> switching to internal LSFG sync path"
                << std::endl;

            externalSupported = false;

            if (fd)
                *fd = -1;
        }
    }
    else {
        externalSupported = false;
    }

    /*
     * =========================================================
     * CASE 2: Fallback BUT NOT DISABLE LSFG
     * =========================================================
     * Instead of disabling LSFG:
     * -> mark fd as invalid
     * -> LSFG will use internal sync mode in context.cpp
     */
    if (!externalSupported) {

        std::cerr
            << "lsfg-vk: semaphore fallback -> INTERNAL LSFG MODE ACTIVE"
            << std::endl;

        const VkSemaphoreCreateInfo fallbackDesc{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
        };

        res = Layer::ovkCreateSemaphore(
            device,
            &fallbackDesc,
            nullptr,
            &semaphoreHandle
        );

        if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
            throw LSFG::vulkan_error(res, "Unable to create fallback semaphore");
        }

        /*
         * IMPORTANT:
         * fd = -1 does NOT mean LSFG off anymore
         * it means "use internal sync path"
         */
        if (fd)
            *fd = -1;
    }

    /*
     * Store semaphore
     */
    this->semaphore =
        std::shared_ptr<VkSemaphore>(
            new VkSemaphore(semaphoreHandle),
            [dev = device](VkSemaphore* semaphoreHandle) {
                Layer::ovkDestroySemaphore(
                    dev,
                    *semaphoreHandle,
                    nullptr
                );
            }
        );
}
