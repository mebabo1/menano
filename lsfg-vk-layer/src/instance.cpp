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
    std::vector<const char*> add_extensions(
        const char* const* existingExtensions,
        size_t count,
        const std::vector<const char*>& requiredExtensions
    ) {
        std::vector<const char*> extensions;

        if (existingExtensions && count > 0) {
            extensions.assign(existingExtensions, existingExtensions + count);
        }

        for (const auto& requiredExtension : requiredExtensions) {
            auto it = std::ranges::find_if(
                extensions,
                [requiredExtension](const char* extension) {
                    return extension && std::string(extension) == requiredExtension;
                }
            );

            if (it == extensions.end())
                extensions.push_back(requiredExtension);
        }

        return extensions;
    }
}

Root::Root() {
    std::cerr << "lsfg-vk: Root ctor enter\n";

    const auto& profile = findProfile(this->config.get(), ls::identify());

    if (!profile.has_value()) {
        std::cerr << "lsfg-vk: profile NOT found -> exit ctor early\n";
        return;
    }

    std::cerr << "lsfg-vk: profile found\n";

    // ✔ FIX 1: optional 안전 대입
    this->active_profile.emplace(profile->second);

    std::cerr << "lsfg-vk: using profile name = '"
              << this->active_profile->name << "'\n";

    switch (profile->first) {
        case ls::IdentType::OVERRIDE:
            std::cerr << "lsfg-vk: ident = OVERRIDE\n";
            break;
        case ls::IdentType::EXECUTABLE:
            std::cerr << "lsfg-vk: ident = EXECUTABLE\n";
            break;
        case ls::IdentType::WINE_EXECUTABLE:
            std::cerr << "lsfg-vk: ident = WINE_EXECUTABLE\n";
            break;
        case ls::IdentType::PROCESS_NAME:
            std::cerr << "lsfg-vk: ident = PROCESS_NAME\n";
            break;
    }

    std::cerr << "lsfg-vk: Root ctor exit\n";
}

bool Root::update() {
    std::cerr << "lsfg-vk: update() enter\n";

    if (!this->config.update()) {
        std::cerr << "lsfg-vk: config.update FAILED\n";
        return false;
    }

    const auto& profile = findProfile(this->config.get(), ls::identify());

    if (profile.has_value()) {
        std::cerr << "lsfg-vk: update profile FOUND\n";
        this->active_profile.emplace(profile->second);
    } else {
        std::cerr << "lsfg-vk: update profile NOT FOUND\n";
        this->active_profile.reset();
    }

    std::cerr << "lsfg-vk: update() exit\n";
    return true;
}

void Root::modifyInstanceCreateInfo(
    VkInstanceCreateInfo& createInfo,
    const std::function<void(void)>& finish
) const {
    std::cerr << "lsfg-vk: modifyInstanceCreateInfo enter\n";

    if (!this->active_profile.has_value()) {
        std::cerr << "lsfg-vk: no active profile -> skip instance patch\n";
        finish();
        return;
    }

    std::cerr << "lsfg-vk: injecting instance extensions\n";

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

    std::cerr << "lsfg-vk: instance extensions injected = "
              << extensions.size() << "\n";

    finish();
    std::cerr << "lsfg-vk: modifyInstanceCreateInfo exit\n";
}

void Root::modifyDeviceCreateInfo(
    VkDeviceCreateInfo& createInfo,
    const std::function<void(void)>& finish
) const {
    std::cerr << "lsfg-vk: modifyDeviceCreateInfo enter\n";

    if (!this->active_profile.has_value()) {
        std::cerr << "lsfg-vk: no active profile -> skip device patch\n";
        finish();
        return;
    }

    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        {
            "VK_KHR_external_memory",
            "VK_KHR_external_semaphore",
            "VK_KHR_timeline_semaphore"
        }
    );

    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    bool isFeatureEnabled = false;

    // ✔ FIX 2: const 제거 안전 캐스트
    auto* featureInfo =
        const_cast<VkBaseInStructure*>(
            reinterpret_cast<const VkBaseInStructure*>(createInfo.pNext)
        );

    std::cerr << "lsfg-vk: walking feature chain\n";

    while (featureInfo) {
        std::cerr << "lsfg-vk: feature sType = " << featureInfo->sType << "\n";

        if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* features = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(featureInfo);
            features->timelineSemaphore = VK_TRUE;
            isFeatureEnabled = true;
        } else if (featureInfo->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* features =
                reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(featureInfo);
            features->timelineSemaphore = VK_TRUE;
            isFeatureEnabled = true;
        }

        featureInfo = const_cast<VkBaseInStructure*>(featureInfo->pNext);
    }

    std::cerr << "lsfg-vk: timeline feature enabled = "
              << (isFeatureEnabled ? "YES" : "NO") << "\n";

    // ✔ FIX 3: lifetime safe (static으로 우회)
    static VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        nullptr,
        VK_TRUE
    };

    timelineFeatures.pNext = createInfo.pNext;

    if (!isFeatureEnabled) {
        std::cerr << "lsfg-vk: forcing timeline feature struct\n";
        createInfo.pNext = &timelineFeatures;
    }

    finish();
    std::cerr << "lsfg-vk: modifyDeviceCreateInfo exit\n";
}

void Root::modifySwapchainCreateInfo(
    const vk::Vulkan& vk,
    VkSwapchainCreateInfoKHR& createInfo,
    const std::function<void(void)>& finish
) const {
    std::cerr << "lsfg-vk: modifySwapchainCreateInfo enter\n";

    if (!this->active_profile.has_value()) {
        std::cerr << "lsfg-vk: no profile -> skip swapchain patch\n";
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
        std::cerr << "lsfg-vk: surface caps FAILED\n";
        throw ls::vulkan_error(res, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR() failed");
    }

    context_ModifySwapchainCreateInfo(*this->active_profile, caps.maxImageCount, createInfo);

    finish();
    std::cerr << "lsfg-vk: modifySwapchainCreateInfo exit\n";
}

void Root::createSwapchainContext(
    const vk::Vulkan& vk,
    VkSwapchainKHR swapchain,
    const SwapchainInfo& info
) {
    std::cerr << "lsfg-vk: createSwapchainContext ENTER\n";

    if (!this->active_profile.has_value()) {
        throw ls::error("attempted to create swapchain context while layer is inactive");
    }

    const auto& profile = *this->active_profile;

    if (!this->backend.has_value()) {
        std::cerr << "lsfg-vk: backend creating\n";

        const auto& global = this->config.get().global();

        setenv("DISABLE_LSFGVK", "1", 1);

        try {
            std::string dll =
                global.dll.has_value() ? *global.dll : ls::findShaderDll();

            this->backend.emplace(
                [gpu = profile.gpu](
                    const std::string& deviceName,
                    std::pair<const std::string&, const std::string&> ids,
                    const std::optional<std::string>& pci
                ) {
                    if (!gpu)
                        return true;

                    return (deviceName == *gpu) ||
                           (ids.first + ":" + ids.second == *gpu) ||
                           (pci && *pci == *gpu);
                },
                dll,
                global.allow_fp16
            );

        } catch (const std::exception& e) {
            unsetenv("DISABLE_LSFGVK");
            throw ls::error("failed to create backend instance", e);
        }

        unsetenv("DISABLE_LSFGVK");
    }

    this->swapchains.emplace(
        swapchain,
        Swapchain(vk, this->backend.mut(), profile, info)
    );

    std::cerr << "lsfg-vk: createSwapchainContext EXIT\n";
}

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    std::cerr << "lsfg-vk: removeSwapchainContext\n";
    this->swapchains.erase(swapchain);
}
