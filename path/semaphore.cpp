#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>
#include <iostream>

using namespace Mini;

/*
 * NOTE:
 * Semaphore creation must be backend-aware.
 * Do NOT assume FD/Win32 support on all drivers.
 */

/* --------------------------------------------------------- */
/* Capability state (global per process)                     */
/* --------------------------------------------------------- */
static bool supportsFdSemaphore = false;
static bool supportsWin32Semaphore = false;

/* --------------------------------------------------------- */
/* Capability setter (MUST be declared in header)            */
/* --------------------------------------------------------- */
void Mini::setSemaphoreCapabilities(bool fd, bool win32) {
    supportsFdSemaphore = fd;
    supportsWin32Semaphore = win32;
}

/* --------------------------------------------------------- */
/* Normal (internal) semaphore                               */
/* --------------------------------------------------------- */
Semaphore::Semaphore(VkDevice device) {

    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };

    VkSemaphore semaphoreHandle{};

    VkResult res = Layer::ovkCreateSemaphore(
        device,
        &desc,
        nullptr,
        &semaphoreHandle);

    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
        throw LSFG::vulkan_error(
            res,
            "Unable to create semaphore");
    }

    this->semaphore =
        std::shared_ptr<VkSemaphore>(
            new VkSemaphore(semaphoreHandle),
            [dev = device](VkSemaphore* s) {
                Layer::ovkDestroySemaphore(dev, *s, nullptr);
            });
}

/* --------------------------------------------------------- */
/* External semaphore (FD / Win32 / fallback)                */
/* --------------------------------------------------------- */
Semaphore::Semaphore(VkDevice device, int* fd) {

    VkSemaphore semaphoreHandle = VK_NULL_HANDLE;
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;

    /*
     * ---------------------------------------------------------
     * 1. Win32 external semaphore (if supported)
     * ---------------------------------------------------------
     */
    if (supportsWin32Semaphore) {

        const VkExportSemaphoreCreateInfo exportInfo{
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
        };

        const VkSemaphoreCreateInfo desc{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &exportInfo
        };

        res = Layer::ovkCreateSemaphore(
            device,
            &desc,
            nullptr,
            &semaphoreHandle);
    }

    /*
     * ---------------------------------------------------------
     * 2. FD external semaphore (Linux / Mesa / Turnip partial)
     * ---------------------------------------------------------
     */
    if (res != VK_SUCCESS && supportsFdSemaphore) {

        const VkExportSemaphoreCreateInfo exportInfo{
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
        };

        const VkSemaphoreCreateInfo desc{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &exportInfo
        };

        res = Layer::ovkCreateSemaphore(
            device,
            &desc,
            nullptr,
            &semaphoreHandle);
    }

    /*
     * ---------------------------------------------------------
     * 3. Fallback internal semaphore
     * ---------------------------------------------------------
     */
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {

        std::cerr
            << "lsfg-vk: external semaphore unsupported, using internal fallback"
            << std::endl;

        const VkSemaphoreCreateInfo fallbackDesc{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
        };

        res = Layer::ovkCreateSemaphore(
            device,
            &fallbackDesc,
            nullptr,
            &semaphoreHandle);

        if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
            throw LSFG::vulkan_error(
                res,
                "Unable to create semaphore");
        }

        if (fd) {
            *fd = -1;
        }
    }

    /*
     * ---------------------------------------------------------
     * 4. Export FD (only if supported)
     * ---------------------------------------------------------
     */
    if (fd && supportsFdSemaphore && semaphoreHandle != VK_NULL_HANDLE) {

        const VkSemaphoreGetFdInfoKHR fdInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = semaphoreHandle,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
        };

        res = Layer::ovkGetSemaphoreFdKHR(device, &fdInfo, fd);

        if (res != VK_SUCCESS || !fd || *fd < 0) {

            std::cerr
                << "lsfg-vk: semaphore fd export failed, falling back to internal sync"
                << std::endl;

            if (fd) {
                *fd = -1;
            }
        }
    }

    /*
     * ---------------------------------------------------------
     * Store handle
     * ---------------------------------------------------------
     */
    this->semaphore =
        std::shared_ptr<VkSemaphore>(
            new VkSemaphore(semaphoreHandle),
            [dev = device](VkSemaphore* s) {
                Layer::ovkDestroySemaphore(dev, *s, nullptr);
            });
}
