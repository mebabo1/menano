/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "instance.hpp"
#include "lsfg-vk-common/helpers/paths.hpp"
#include "swapchain.hpp"
#include "lsfg-vk-common/configuration/detection.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <stdlib.h>
#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;

namespace {

#define LOG(tag, msg) \
    std::cerr << "[LSFG][" << tag << "] " << msg << std::endl

// =====================================================
// IMPORTANT:
// Persistent extension storage.
// Wine crashes if ppEnabledExtensionNames points
// to dead stack memory.
// =====================================================

thread_local std::vector<const char*> g_instanceExtensions;
thread_local std::vector<const char*> g_deviceExtensions;

/// helper function to add required extensions
std::vector<const char*> add_extensions(
    const char* const* existingExtensions,
    size_t count,
    const std::vector<const char*>& requiredExtensions)
{
    std::vector<const char*> extensions;

    extensions.reserve(
        count + requiredExtensions.size());

    for (size_t i = 0; i < count; i++) {

        if (existingExtensions[i]) {
            extensions.push_back(
                existingExtensions[i]);
        }
    }

    for (const auto& requiredExtension
            : requiredExtensions)
    {
        auto it = std::ranges::find_if(
            extensions,
            [requiredExtension](
                const char* extension)
            {
                return extension
                    && std::string(extension)
                        == requiredExtension;
            });

        if (it == extensions.end()) {

            LOG("Extensions",
                "Adding extension: "
                << requiredExtension);

            extensions.push_back(
                requiredExtension);
        }
    }

    return extensions;
}

} // namespace

// =====================================================
// ROOT
// =====================================================

Root::Root()
{
    LOG("Root", "ctor enter");

    const auto& profile =
        findProfile(
            this->config.get(),
            ls::identify());

    if (!profile.has_value()) {

        LOG("Root",
            "no profile found");

        return;
    }

    this->active_profile =
        profile->second;

    std::cerr
        << "lsfg-vk: using profile with name '"
        << this->active_profile->name
        << "' ";

    switch (profile->first) {

        case ls::IdentType::OVERRIDE:
            std::cerr
                << "(identified via override)\n";
            break;

        case ls::IdentType::EXECUTABLE:
            std::cerr
                << "(identified via executable)\n";
            break;

        case ls::IdentType::WINE_EXECUTABLE:
            std::cerr
                << "(identified via wine executable)\n";
            break;

        case ls::IdentType::PROCESS_NAME:
            std::cerr
                << "(identified via process name)\n";
            break;
    }

    LOG("Root", "ctor exit");
}

bool Root::update()
{
    if (!this->config.update()) {
        return false;
    }

    const auto& profile =
        findProfile(
            this->config.get(),
            ls::identify());

    if (profile.has_value()) {
        this->active_profile =
            profile->second;
    } else {
        this->active_profile =
            std::nullopt;
    }

    return true;
}

// =====================================================
// INSTANCE
// =====================================================

void Root::modifyInstanceCreateInfo(
    VkInstanceCreateInfo& createInfo,
    const std::function<void(void)>& finish) const
{
    LOG("Instance", "modify enter");

    if (!this->active_profile.has_value()) {

        finish();
        return;
    }

    // =================================================
    // IMPORTANT:
    // Must persist after function returns.
    // =================================================

    g_instanceExtensions =
        add_extensions(
            createInfo.ppEnabledExtensionNames,
            createInfo.enabledExtensionCount,
            {
                "VK_KHR_get_physical_device_properties2",
                "VK_KHR_external_memory_capabilities",
                "VK_KHR_external_semaphore_capabilities"
            });

    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(
            g_instanceExtensions.size());

    createInfo.ppEnabledExtensionNames =
        g_instanceExtensions.data();

    LOG("Instance",
        "extension count = "
        << createInfo.enabledExtensionCount);

    for (uint32_t i = 0;
            i < createInfo.enabledExtensionCount;
            i++)
    {
        LOG("Instance",
            "extension[" << i << "] = "
            << createInfo
                .ppEnabledExtensionNames[i]);
    }

    finish();

    LOG("Instance", "modify exit");
}

// =====================================================
// DEVICE
// =====================================================

void Root::modifyDeviceCreateInfo(
    VkDeviceCreateInfo& createInfo,
    const std::function<void(void)>& finish) const
{
    LOG("Device", "modify enter");

    if (!this->active_profile.has_value()) {

        finish();
        return;
    }

    // =================================================
    // IMPORTANT:
    // Must persist after function returns.
    // =================================================

    g_deviceExtensions =
        add_extensions(
            createInfo.ppEnabledExtensionNames,
            createInfo.enabledExtensionCount,
            {
                "VK_KHR_external_memory",
                "VK_KHR_external_memory_fd",
                "VK_KHR_external_semaphore",
                "VK_KHR_external_semaphore_fd",
                "VK_KHR_timeline_semaphore"
            });

    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(
            g_deviceExtensions.size());

    createInfo.ppEnabledExtensionNames =
        g_deviceExtensions.data();

    LOG("Device",
        "extension count = "
        << createInfo.enabledExtensionCount);

    for (uint32_t i = 0;
            i < createInfo.enabledExtensionCount;
            i++)
    {
        LOG("Device",
            "extension[" << i << "] = "
            << createInfo
                .ppEnabledExtensionNames[i]);
    }

    // =================================================
    // SAFE pNext traversal
    // =================================================

    bool foundTimeline = false;

    auto* featureInfo =
        reinterpret_cast<VkBaseInStructure*>(
            const_cast<void*>(
                createInfo.pNext));

    uint32_t index = 0;

    while (featureInfo) {

        LOG("Device",
            "pNext[" << index
            << "] sType="
            << featureInfo->sType
            << " ptr="
            << featureInfo);

        switch (featureInfo->sType) {

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            {
                auto* features =
                    reinterpret_cast<
                        VkPhysicalDeviceVulkan12Features*>(
                            featureInfo);

                LOG("Device",
                    "Found Vulkan12Features");

                features->timelineSemaphore =
                    VK_TRUE;

                foundTimeline = true;
                break;
            }

            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            {
                auto* features =
                    reinterpret_cast<
                        VkPhysicalDeviceTimelineSemaphoreFeatures*>(
                            featureInfo);

                LOG("Device",
                    "Found TimelineSemaphoreFeatures");

                features->timelineSemaphore =
                    VK_TRUE;

                foundTimeline = true;
                break;
            }

            default:
                break;
        }

        featureInfo =
            reinterpret_cast<VkBaseInStructure*>(
                const_cast<void*>(
                    featureInfo->pNext));

        index++;
    }

    // =================================================
    // DO NOT inject custom pNext structs on Wine.
    // =================================================

    if (!foundTimeline) {

        LOG("Device",
            "timeline semaphore feature NOT found");

        LOG("Device",
            "Skipping pNext injection for Wine safety");
    }

    finish();

    LOG("Device", "modify exit");
}

// =====================================================
// SWAPCHAIN
// =====================================================

void Root::modifySwapchainCreateInfo(
    const vk::Vulkan& vk,
    VkSwapchainCreateInfoKHR& createInfo,
    const std::function<void(void)>& finish) const
{
    LOG("Swapchain", "modify enter");

    if (!this->active_profile.has_value()) {

        finish();
        return;
    }

    VkSurfaceCapabilitiesKHR caps{};

    auto res =
        vk.fi()
            .GetPhysicalDeviceSurfaceCapabilitiesKHR(
                vk.physdev(),
                createInfo.surface,
                &caps);

    if (res != VK_SUCCESS) {

        throw ls::vulkan_error(
            res,
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR() failed");
    }

    LOG("Swapchain",
        "maxImageCount="
        << caps.maxImageCount);

    context_ModifySwapchainCreateInfo(
        *this->active_profile,
        caps.maxImageCount,
        createInfo);

    finish();

    LOG("Swapchain", "modify exit");
}

// =====================================================
// BACKEND
// =====================================================

void Root::createSwapchainContext(
    const vk::Vulkan& vk,
    VkSwapchainKHR swapchain,
    const SwapchainInfo& info)
{
    LOG("Backend",
        "createSwapchainContext enter");

    if (!this->active_profile.has_value()) {

        throw ls::error(
            "attempted to create swapchain context while layer is inactive");
    }

    const auto& profile =
        *this->active_profile;

    if (!this->backend.has_value()) {

        const auto& global =
            this->config.get().global();

        setenv(
            "DISABLE_LSFGVK",
            "1",
            1);

        try {

            std::string dll{};

            if (global.dll.has_value()) {
                dll = *global.dll;
            } else {
                dll =
                    ls::findShaderDll()
                        .string();
            }

            LOG("Backend",
                "dll = " << dll);

            this->backend.emplace(

                [gpu = profile.gpu](
                    const std::string& deviceName,
                    std::pair<
                        const std::string&,
                        const std::string&> ids,
                    const std::optional<std::string>& pci)
                {
                    std::cerr
                        << "lsfg-vk: probing device\n"
                        << "  name: "
                        << deviceName
                        << "\n"
                        << "  ids: "
                        << ids.first
                        << ":"
                        << ids.second
                        << "\n"
                        << "  pci: "
                        << (pci ? *pci : "none")
                        << "\n";

                    if (!gpu) {
                        return true;
                    }

                    return
                        (deviceName == *gpu)
                        || (ids.first + ":"
                            + ids.second
                            == *gpu)
                        || (pci
                            && *pci == *gpu);
                },

                dll,
                global.allow_fp16);
        }
        catch (const std::exception& e) {

            unsetenv("DISABLE_LSFGVK");

            throw ls::error(
                "failed to create backend instance",
                e);
        }

        unsetenv("DISABLE_LSFGVK");
    }

    this->swapchains.emplace(
        swapchain,
        Swapchain(
            vk,
            this->backend.mut(),
            profile,
            info));

    LOG("Backend",
        "createSwapchainContext exit");
}

// =====================================================
// CLEANUP
// =====================================================

void Root::removeSwapchainContext(
    VkSwapchainKHR swapchain)
{
    LOG("Swapchain",
        "remove context");

    this->swapchains.erase(
        swapchain);
}
