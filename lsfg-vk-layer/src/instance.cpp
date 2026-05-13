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

    for (const auto& req : requiredExtensions) {
        auto it = std::ranges::find_if(extensions,
            [&](const char* ext) {
                return ext && std::string(ext) == req;
            });

        if (it == extensions.end())
            extensions.push_back(req);
    }

    return extensions;
}

} // namespace

// =====================================================
// ROOT
// =====================================================

Root::Root() {
    LOG("Root", "ctor");

    const auto& profile = findProfile(this->config.get(), ls::identify());

    if (!profile.has_value()) {
        LOG("Root", "no profile");
        return;
    }

    this->active_profile = profile->second;

    LOG("Root", "profile = " << this->active_profile->name);
}

bool Root::update() {
    LOG("Root", "update");

    if (!this->config.update())
        return false;

    const auto& profile = findProfile(this->config.get(), ls::identify());

    if (profile.has_value()) {
        this->active_profile = profile->second;
        LOG("Root", "profile updated");
    } else {
        this->active_profile = std::nullopt;
        LOG("Root", "profile cleared");
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
    LOG("Instance", "modify");

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
    LOG("Device", "modify");

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

    auto* feature =
        reinterpret_cast<VkBaseInStructure*>(const_cast<void*>(createInfo.pNext));

    int idx = 0;
    while (feature) {
        LOG("Device", "pNext[" << idx++ << "] sType=" << feature->sType);

        if (feature->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan12Features*)feature;
            f->timelineSemaphore = VK_TRUE;
            foundTimeline = true;
        }

        if (feature->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* f = (VkPhysicalDeviceTimelineSemaphoreFeatures*)feature;
            f->timelineSemaphore = VK_TRUE;
            foundTimeline = true;
        }

        feature = (VkBaseInStructure*)feature->pNext;
    }

    // ❗ FIX: const void* → void*
    static VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        nullptr,
        VK_TRUE
    };

    if (!foundTimeline) {
        LOG("Device", "inject timeline feature");

        timelineFeatures.pNext = const_cast<void*>(createInfo.pNext);
        createInfo.pNext = &timelineFeatures;
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

    LOG("Swapchain", "maxImageCount=" << caps.maxImageCount);

    context_ModifySwapchainCreateInfo(
        *this->active_profile,
        caps.maxImageCount,
        createInfo
    );

    finish();
}

// =====================================================
// BACKEND / SWAPCHAIN CONTEXT
// =====================================================

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
        LOG("Backend", "init");

        const auto& global = this->config.get().global();

        setenv("DISABLE_LSFGVK", "1", 1);

        try {
            std::filesystem::path dllPath =
                global.dll.has_value()
                    ? std::filesystem::path(*global.dll)
                    : ls::findShaderDll();

            std::string dll = dllPath.string();

            LOG("Backend", "dll=" << dll);

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

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    LOG("Backend", "swapchain remove");
    this->swapchains.erase(swapchain);
}
