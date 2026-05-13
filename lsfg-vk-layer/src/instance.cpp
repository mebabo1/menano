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
    LOG("Extensions", "merge start");

    std::vector<const char*> extensions(count);
    std::copy_n(existingExtensions, count, extensions.data());

    for (const auto& requiredExtension : requiredExtensions) {
        auto it = std::ranges::find_if(extensions,
            [requiredExtension](const char* extension) {
                return extension && std::string(extension) == requiredExtension;
            });

        if (it == extensions.end()) {
            LOG("Extensions", "adding " << requiredExtension);
            extensions.push_back(requiredExtension);
        }
    }

    LOG("Extensions", "final count = " << extensions.size());
    return extensions;
}

} // namespace

Root::Root() {
    LOG("Root", "ctor start");

    const auto& profile = findProfile(this->config.get(), ls::identify());

    if (!profile.has_value()) {
        LOG("Root", "no active profile found");
        return;
    }

    this->active_profile = profile->second;

    LOG("Root", "profile = " << this->active_profile->name);

    switch (profile->first) {
        case ls::IdentType::OVERRIDE:
            LOG("Root", "identified via OVERRIDE");
            break;
        case ls::IdentType::EXECUTABLE:
            LOG("Root", "identified via EXECUTABLE");
            break;
        case ls::IdentType::WINE_EXECUTABLE:
            LOG("Root", "identified via WINE_EXECUTABLE");
            break;
        case ls::IdentType::PROCESS_NAME:
            LOG("Root", "identified via PROCESS_NAME");
            break;
    }

    LOG("Root", "ctor end");
}

bool Root::update() {
    LOG("Root", "update called");

    if (!this->config.update()) {
        LOG("Root", "config update skipped/failed");
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
        LOG("Instance", "inactive -> skip");
        finish();
        return;
    }

    LOG("Instance", "original extension count = " << createInfo.enabledExtensionCount);

    for (uint32_t i = 0; i < createInfo.enabledExtensionCount; i++) {
        LOG("Instance", std::string("orig ext = ") + createInfo.ppEnabledExtensionNames[i]);
    }

    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        {
            "VK_KHR_get_physical_device_properties2",
            "VK_KHR_external_memory_capabilities",
            "VK_KHR_external_semaphore_capabilities"
        }
    );

    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    for (auto& e : extensions) {
        LOG("Instance", std::string("final ext = ") + e);
    }

    LOG("Instance", "modify end");
    finish();
}

void Root::modifyDeviceCreateInfo(
    VkDeviceCreateInfo& createInfo,
    const std::function<void(void)>& finish) const
{
    LOG("Device", "modify start");

    if (!this->active_profile.has_value()) {
        LOG("Device", "inactive -> skip");
        finish();
        return;
    }

    LOG("Device", "extension count = " << createInfo.enabledExtensionCount);

    for (uint32_t i = 0; i < createInfo.enabledExtensionCount; i++) {
        LOG("Device", std::string("ext = ") + createInfo.ppEnabledExtensionNames[i]);
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

    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    bool isFeatureEnabled = false;

    auto* featureInfo =
        reinterpret_cast<VkBaseInStructure*>(const_cast<void*>(createInfo.pNext));

    int idx = 0;
    while (featureInfo) {
        LOG("Device", "pNext[" << idx++ << "] sType=" << featureInfo->sType);

        if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* features =
                reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(featureInfo);

            features->timelineSemaphore = VK_TRUE;
            isFeatureEnabled = true;

            LOG("Device", "Vulkan1.2 timeline enabled");
        }

        if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* features =
                reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(featureInfo);

            features->timelineSemaphore = VK_TRUE;
            isFeatureEnabled = true;

            LOG("Device", "timeline semaphore feature enabled");
        }

        featureInfo =
            const_cast<VkBaseInStructure*>(featureInfo->pNext);
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .pNext = const_cast<void*>(createInfo.pNext),
        .timelineSemaphore = VK_TRUE
    };

    if (!isFeatureEnabled) {
        createInfo.pNext = &timelineFeatures;
        LOG("Device", "timeline feature injected manually");
    }

    LOG("Device", "modify end");
    finish();
}

void Root::modifySwapchainCreateInfo(
    const vk::Vulkan& vk,
    VkSwapchainCreateInfoKHR& createInfo,
    const std::function<void(void)>& finish) const
{
    LOG("Swapchain", "modify start");

    if (!this->active_profile.has_value()) {
        LOG("Swapchain", "inactive -> skip");
        finish();
        return;
    }

    VkSurfaceCapabilitiesKHR caps{};
    auto res = vk.fi().GetPhysicalDeviceSurfaceCapabilitiesKHR(
        vk.physdev(),
        createInfo.surface,
        &caps
    );

    if (res != VK_SUCCESS) {
        LOG("Swapchain", "cap query failed");
        throw ls::vulkan_error(res, "surface caps failed");
    }

    LOG("Swapchain", "maxImageCount = " << caps.maxImageCount);

    context_ModifySwapchainCreateInfo(
        *this->active_profile,
        caps.maxImageCount,
        createInfo
    );

    LOG("Swapchain", "modify end");
    finish();
}

void Root::createSwapchainContext(
    const vk::Vulkan& vk,
    VkSwapchainKHR swapchain,
    const SwapchainInfo& info)
{
    LOG("Backend", "swapchain context create");

    if (!this->active_profile.has_value()) {
        LOG("Backend", "no active profile -> fail");
        throw ls::error("inactive layer");
    }

    const auto& profile = *this->active_profile;

    if (!this->backend.has_value()) {
        LOG("Backend", "initializing backend");

        const auto& global = this->config.get().global();

        setenv("DISABLE_LSFGVK", "1", 1);

        try {
            std::string dll =
                global.dll.has_value()
                    ? *global.dll
                    : ls::findShaderDll();

            LOG("Backend", "dll = " << dll);

            this->backend.emplace(
                [gpu = profile.gpu](
                    const std::string& deviceName,
                    std::pair<const std::string&, const std::string&> ids,
                    const std::optional<std::string>& pci)
                {
                    LOG("Backend", "probe device = " << deviceName);

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
            LOG("Backend", "init failed");
            throw ls::error("backend init failed", e);
        }

        unsetenv("DISABLE_LSFGVK");
    }

    this->swapchains.emplace(
        swapchain,
        Swapchain(vk, this->backend.mut(), profile, info)
    );

    LOG("Backend", "swapchain registered");
}

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    LOG("Backend", "swapchain removed");
    this->swapchains.erase(swapchain);
}
