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

    void dump_extensions(const char* label, const char* const* exts, uint32_t count) {
        std::cerr << "\n[" << label << "] count=" << count << " ptr=" << exts << "\n";
        for (uint32_t i = 0; i < count; i++) {
            if (exts && exts[i]) {
                std::cerr << "  [" << i << "] " << exts[i] << "\n";
            } else {
                std::cerr << "  [" << i << "] <null>\n";
            }
        }
    }

    void dump_pnext_chain(const void* pNext) {
        std::cerr << "\n[pNext chain dump]\n";
        const VkBaseInStructure* node =
            reinterpret_cast<const VkBaseInStructure*>(pNext);

        int idx = 0;
        while (node) {
            std::cerr << "  node[" << idx << "] sType=" << node->sType
                      << " pNext=" << node->pNext << "\n";
            node = node->pNext;
            idx++;
        }

        if (idx == 0) {
            std::cerr << "  <empty chain>\n";
        }
    }

    std::vector<const char*> add_extensions(
        const char* const* existingExtensions,
        size_t count,
        const std::vector<const char*>& requiredExtensions
    ) {
        std::vector<const char*> extensions(count);
        std::copy_n(existingExtensions, count, extensions.data());

        for (const auto& requiredExtension : requiredExtensions) {
            auto it = std::ranges::find_if(
                extensions,
                [requiredExtension](const char* extension) {
                    return std::string(extension) == std::string(requiredExtension);
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

    this->active_profile = profile->second;

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
        this->active_profile = profile->second;
    } else {
        this->active_profile = std::nullopt;
    }

    std::cerr << "lsfg-vk: update() exit\n";
    return true;
}

void Root::modifyInstanceCreateInfo(
    VkInstanceCreateInfo& createInfo,
    const std::function<void(void)>& finish
) const {
    std::cerr << "lsfg-vk: modifyInstanceCreateInfo enter\n";

    dump_extensions(
        "incoming instance extensions",
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount
    );

    if (!this->active_profile.has_value()) {
        finish();
        return;
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

    dump_extensions(
        "after add_extensions (instance)",
        extensions.data(),
        (uint32_t)extensions.size()
    );

    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    std::cerr << "lsfg-vk: instance extensions injected = "
              << extensions.size() << "\n";

    finish();
}

void Root::modifyDeviceCreateInfo(
    VkDeviceCreateInfo& createInfo,
    const std::function<void(void)>& finish
) const {
    std::cerr << "lsfg-vk: modifyDeviceCreateInfo enter\n";

    dump_extensions(
        "incoming device extensions",
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount
    );

    if (!this->active_profile.has_value()) {
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

    dump_extensions(
        "after add_extensions (device)",
        extensions.data(),
        (uint32_t)extensions.size()
    );

    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    dump_pnext_chain(createInfo.pNext);

    bool isFeatureEnabled = false;

    auto* featureInfo =
        reinterpret_cast<VkBaseInStructure*>(const_cast<void*>(createInfo.pNext));

    while (featureInfo) {
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

    std::cerr << "timeline enabled = " << isFeatureEnabled << "\n";

    finish();
}

void Root::modifySwapchainCreateInfo(
    const vk::Vulkan& vk,
    VkSwapchainCreateInfoKHR& createInfo,
    const std::function<void(void)>& finish
) const {
    std::cerr << "lsfg-vk: modifySwapchainCreateInfo enter\n";

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

    if (res != VK_SUCCESS) {
        throw ls::vulkan_error(res, "surface caps failed");
    }

    context_ModifySwapchainCreateInfo(*this->active_profile, caps.maxImageCount, createInfo);

    finish();
}

void Root::createSwapchainContext(
    const vk::Vulkan& vk,
    VkSwapchainKHR swapchain,
    const SwapchainInfo& info
) {
    std::cerr << "lsfg-vk: createSwapchainContext ENTER\n";

    if (!this->active_profile.has_value()) {
        throw ls::error("inactive layer");
    }

    const auto& profile = *this->active_profile;

    if (!this->backend.has_value()) {
        const auto& global = this->config.get().global();

        setenv("DISABLE_LSFGVK", "1", 1);

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

        unsetenv("DISABLE_LSFGVK");
    }

    this->swapchains.emplace(
        swapchain,
        Swapchain(vk, this->backend.mut(), profile, info)
    );
}

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    this->swapchains.erase(swapchain);
}
