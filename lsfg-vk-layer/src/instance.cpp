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
    std::vector<const char*> extensions(count);
    std::copy_n(existingExtensions, count, extensions.data());

    for (const auto& requiredExtension : requiredExtensions) {
        auto it = std::ranges::find_if(extensions,
            [&](const char* extension) {
                return extension && std::string(extension) == requiredExtension;
            });

        if (it == extensions.end())
            extensions.push_back(requiredExtension);
    }

    return extensions;
}

// =====================================================
// SAFE STATIC FEATURE (NO STACK / NO NEW)
// =====================================================
static VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatureStatic{
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
    nullptr,
    VK_TRUE
};

} // namespace

// =====================================================
// ROOT
// =====================================================

Root::Root() {
    const auto& profile = findProfile(this->config.get(), ls::identify());

    if (!profile.has_value())
        return;

    this->active_profile = profile->second;

    std::cerr << "lsfg-vk: using profile '" << this->active_profile->name << "'\n";
}

bool Root::update() {
    if (!this->config.update())
        return false;

    const auto& profile = findProfile(this->config.get(), ls::identify());

    this->active_profile =
        profile.has_value() ? std::optional(profile->second) : std::nullopt;

    return true;
}

// =====================================================
// INSTANCE
// =====================================================

void Root::modifyInstanceCreateInfo(
    VkInstanceCreateInfo& createInfo,
    const std::function<void(void)>& finish) const
{
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

    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();

    finish();
}

// =====================================================
// DEVICE
// =====================================================

void Root::modifyDeviceCreateInfo(
    VkDeviceCreateInfo& createInfo,
    const std::function<void(void)>& finish) const
{
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

    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();

    bool foundTimeline = false;

    // =====================================================
    // SAFE CONST-CORRECT pNext traversal
    // =====================================================
    const VkBaseInStructure* featureInfo =
        reinterpret_cast<const VkBaseInStructure*>(createInfo.pNext);

    while (featureInfo) {
        if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan12Features*)featureInfo;
            f->timelineSemaphore = VK_TRUE;
            foundTimeline = true;
        }

        if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* f = (VkPhysicalDeviceTimelineSemaphoreFeatures*)featureInfo;
            f->timelineSemaphore = VK_TRUE;
            foundTimeline = true;
        }

        featureInfo =
            reinterpret_cast<const VkBaseInStructure*>(featureInfo->pNext);
    }

    // =====================================================
    // SAFE STATIC injection (NO new, NO stack)
    // =====================================================
    if (!foundTimeline) {
        timelineFeatureStatic.pNext = createInfo.pNext;
        createInfo.pNext = &timelineFeatureStatic;
    }

    finish();
}

// =====================================================
// SWAPCHAIN
// =====================================================

void Root::modifySwapchainCreateInfo(
    const vk::Vulkan& vk,
    VkSwapchainCreateInfoKHR& createInfo,
    const std::function<void(void)>& finish) const
{
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

    context_ModifySwapchainCreateInfo(
        *this->active_profile,
        caps.maxImageCount,
        createInfo
    );

    finish();
}

// =====================================================
// BACKEND
// =====================================================

void Root::createSwapchainContext(
    const vk::Vulkan& vk,
    VkSwapchainKHR swapchain,
    const SwapchainInfo& info)
{
    if (!this->active_profile.has_value())
        throw ls::error("inactive layer");

    const auto& profile = *this->active_profile;

    if (!this->backend.has_value()) {
        const auto& global = this->config.get().global();

        setenv("DISABLE_LSFGVK", "1", 1);

        try {
            std::string dll =
                global.dll.has_value()
                    ? *global.dll
                    : ls::findShaderDll().string();

            this->backend.emplace(
                [gpu = profile.gpu](
                    const std::string& name,
                    std::pair<const std::string&, const std::string&> ids,
                    const std::optional<std::string>& pci)
                {
                    if (!gpu) return true;

                    return name == *gpu
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

// =====================================================
// CLEANUP
// =====================================================

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    this->swapchains.erase(swapchain);
}
