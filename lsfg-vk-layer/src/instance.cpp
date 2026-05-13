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
#include <filesystem>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;

namespace {

#define LOG(tag, msg) std::cerr << "[LSFG][" << tag << "] " << msg << "\n"

std::vector<const char*> add_extensions(
    const char* const* existingExtensions,
    size_t count,
    const std::vector<const char*>& requiredExtensions)
{
    std::vector<const char*> extensions(count);
    std::copy_n(existingExtensions, count, extensions.data());

    for (const auto& requiredExtension : requiredExtensions) {
        auto it = std::ranges::find_if(extensions,
            [requiredExtension](const char* extension) {
                return extension && std::string(extension) == requiredExtension;
            });

        if (it == extensions.end())
            extensions.push_back(requiredExtension);
    }

    return extensions;
}

} // namespace

Root::Root() {
    LOG("Root", "ctor start");

    const auto& profile = findProfile(this->config.get(), ls::identify());

    if (!profile.has_value()) {
        LOG("Root", "no profile found");
        return;
    }

    this->active_profile = profile->second;

    LOG("Root", "profile = " << this->active_profile->name);

    switch (profile->first) {
        case ls::IdentType::OVERRIDE:
            LOG("Root", "IDENT OVERRIDE");
            break;
        case ls::IdentType::EXECUTABLE:
            LOG("Root", "IDENT EXECUTABLE");
            break;
        case ls::IdentType::WINE_EXECUTABLE:
            LOG("Root", "IDENT WINE_EXECUTABLE");
            break;
        case ls::IdentType::PROCESS_NAME:
            LOG("Root", "IDENT PROCESS_NAME");
            break;
    }
}

bool Root::update() {
    LOG("Root", "update");

    if (!this->config.update()) {
        LOG("Root", "config unchanged/failed");
        return false;
    }

    const auto& profile = findProfile(this->config.get(), ls::identify());

    if (profile.has_value()) {
        this->active_profile = profile->second;
        LOG("Root", "profile updated = " << this->active_profile->name);
    } else {
        this->active_profile = std::nullopt;
        LOG("Root", "profile cleared");
    }

    return true;
}

void Root::modifyInstanceCreateInfo(
    VkInstanceCreateInfo& createInfo,
    const std::function<void(void)>& finish) const
{
    LOG("Instance", "modify start");

    if (!this->active_profile.has_value()) {
        LOG("Instance", "inactive");
        finish();
        return;
    }

    LOG("Instance", "extension count = " << createInfo.enabledExtensionCount);

    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        {
            "VK_KHR_get_physical_device_properties2",
            "VK_KHR_external_memory_capabilities",
            "VK_KHR_external_semaphore_capabilities"
        }
    );

    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    LOG("Instance", "final extension count = " << extensions.size());

    finish();
}

void Root::modifyDeviceCreateInfo(
    VkDeviceCreateInfo& createInfo,
    const std::function<void(void)>& finish) const
{
    LOG("Device", "modify start");

    if (!this->active_profile.has_value()) {
        LOG("Device", "inactive");
        finish();
        return;
    }

    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        {
            "VK_KHR_external_memory",
            "VK_KHR_external_memory_fd",
            "VK_KHR_external_semaphore",
            "VK_KHR_external_semaphore_fd",
            "VK_KHR_timeline_semaphore"
        }
    );

    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    bool timelineEnabled = false;

    auto* featureInfo =
        reinterpret_cast<VkBaseInStructure*>(const_cast<void*>(createInfo.pNext));

    int idx = 0;
    while (featureInfo) {
        LOG("Device", "pNext[" << idx++ << "] sType=" << featureInfo->sType);

        if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan12Features*)featureInfo;
            f->timelineSemaphore = VK_TRUE;
            timelineEnabled = true;
        }

        if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* f = (VkPhysicalDeviceTimelineSemaphoreFeatures*)featureInfo;
            f->timelineSemaphore = VK_TRUE;
            timelineEnabled = true;
        }

        featureInfo = (VkBaseInStructure*)featureInfo->pNext;
    }

    if (!timelineEnabled) {
        LOG("Device", "inject timeline feature");

        static VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
            nullptr,
            VK_TRUE
        };

        timelineFeatures.pNext = createInfo.pNext;
        createInfo.pNext = &timelineFeatures;
    }

    finish();
}

void Root::modifySwapchainCreateInfo(
    const vk::Vulkan& vk,
    VkSwapchainCreateInfoKHR& createInfo,
    const std::function<void(void)>& finish) const
{
    LOG("Swapchain", "modify");

    if (!this->active_profile.has_value()) {
        finish();
        return;
    }

    VkSurfaceCapabilitiesKHR caps{};
    auto res = vk.fi().GetPhysicalDeviceSurfaceCapabilitiesKHR(
        vk.physdev(),
        createInfo.surface,
        &caps
    );

    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "surface caps failed");

    LOG("Swapchain", "maxImageCount = " << caps.maxImageCount);

    context_ModifySwapchainCreateInfo(
        *this->active_profile,
        caps.maxImageCount,
        createInfo
    );

    finish();
}

void Root::createSwapchainContext(
    const vk::Vulkan& vk,
    VkSwapchainKHR swapchain,
    const SwapchainInfo& info)
{
    LOG("Backend", "swapchain create");

    if (!this->active_profile.has_value())
        throw ls::error("inactive layer");

    const auto& profile = *this->active_profile;

    if (!this->backend.has_value()) {
        LOG("Backend", "init backend");

        const auto& global = this->config.get().global();

        setenv("DISABLE_LSFGVK", "1", 1);

        try {
            std::filesystem::path dllPath =
                global.dll.has_value()
                    ? std::filesystem::path(*global.dll)
                    : ls::findShaderDll();

            std::string dll = dllPath.string();

            LOG("Backend", "dll = " << dll);

            this->backend.emplace(
                [gpu = profile.gpu](
                    const std::string& deviceName,
                    std::pair<const std::string&, const std::string&> ids,
                    const std::optional<std::string>& pci)
                {
                    if (!gpu)
                        return true;

                    return (deviceName == *gpu)
                        || (ids.first + ":" + ids.second == *gpu)
                        || (pci && *pci == *gpu);
                },
                dll,
                global.allow_fp16
            );
        }
        catch (const std::exception& e) {
            unsetenv("DISABLE_LSFGVK");
            throw ls::error("backend init failed", e);
        }

        unsetenv("DISABLE_LSFGVK");
    }

    this->swapchains.emplace(
        swapchain,
        Swapchain(vk, this->backend.mut(), profile, info)
    );
}

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    LOG("Backend", "swapchain remove");
    this->swapchains.erase(swapchain);
}
