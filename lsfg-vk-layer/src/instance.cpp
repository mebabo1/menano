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
            std::cerr << "  [" << i << "] "
                      << (exts && exts[i] ? exts[i] : "<null>") << "\n";
        }
    }

    void dump_pnext_chain(const void* pNext) {
        std::cerr << "\n[pNext chain]\n";
        const VkBaseInStructure* node =
            reinterpret_cast<const VkBaseInStructure*>(pNext);

        int i = 0;
        while (node) {
            std::cerr << "  [" << i++ << "] sType=" << node->sType
                      << " pNext=" << node->pNext << "\n";
            node = node->pNext;
        }

        if (i == 0)
            std::cerr << "  <empty>\n";
    }

    void dump_surface_caps(const VkSurfaceCapabilitiesKHR& caps) {
        std::cerr << "\n[surface caps]\n";
        std::cerr << "  minImageCount=" << caps.minImageCount << "\n";
        std::cerr << "  maxImageCount=" << caps.maxImageCount << "\n";
        std::cerr << "  currentExtent=" << caps.currentExtent.width
                  << "x" << caps.currentExtent.height << "\n";
    }

    std::vector<const char*> add_extensions(
        const char* const* existingExtensions,
        size_t count,
        const std::vector<const char*>& requiredExtensions
    ) {
        std::vector<const char*> extensions;

        if (existingExtensions && count > 0) {
            extensions.assign(existingExtensions, existingExtensions + count);
        }

        for (const auto& req : requiredExtensions) {
            auto it = std::find_if(
                extensions.begin(),
                extensions.end(),
                [&](const char* e) { return e && req == e; }
            );

            if (it == extensions.end())
                extensions.push_back(req);
        }

        return extensions;
    }
}

Root::Root() {
    std::cerr << "\n[Root] ctor enter\n";

    const auto& profile = findProfile(this->config.get(), ls::identify());

    std::cerr << "[Root] profile lookup result = "
              << (profile.has_value() ? "OK" : "FAIL") << "\n";

    if (!profile.has_value()) {
        std::cerr << "[Root] ctor exit (no profile)\n";
        return;
    }

    this->active_profile = profile->second;

    std::cerr << "[Root] profile name = "
              << this->active_profile->name << "\n";

    std::cerr << "[Root] ctor exit\n";
}

bool Root::update() {
    std::cerr << "\n[Root] update enter\n";

    bool ok = this->config.update();
    std::cerr << "[Root] config.update = " << ok << "\n";

    const auto& profile = findProfile(this->config.get(), ls::identify());

    std::cerr << "[Root] profile after update = "
              << (profile.has_value() ? "OK" : "NONE") << "\n";

    if (profile.has_value())
        this->active_profile = profile->second;
    else
        this->active_profile = std::nullopt;

    std::cerr << "[Root] update exit\n";
    return ok;
}

void Root::modifyInstanceCreateInfo(
    VkInstanceCreateInfo& createInfo,
    const std::function<void(void)>& finish
) const {
    std::cerr << "\n[Instance] enter\n";

    dump_extensions(
        "incoming instance",
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount
    );

    if (!this->active_profile.has_value()) {
        std::cerr << "[Instance] no profile\n";
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

    dump_extensions("after add (instance)", extensions.data(), (uint32_t)extensions.size());

    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    std::cerr << "[Instance] final count = " << extensions.size() << "\n";

    finish();
    std::cerr << "[Instance] exit\n";
}

void Root::modifyDeviceCreateInfo(
    VkDeviceCreateInfo& createInfo,
    const std::function<void(void)>& finish
) const {
    std::cerr << "\n[Device] enter\n";

    dump_extensions(
        "incoming device",
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount
    );

    if (!this->active_profile.has_value()) {
        std::cerr << "[Device] no profile\n";
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

    dump_extensions("after add (device)", extensions.data(), (uint32_t)extensions.size());

    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    dump_pnext_chain(createInfo.pNext);

    bool timelineFound = false;

    auto* node = reinterpret_cast<VkBaseInStructure*>(const_cast<void*>(createInfo.pNext));

    std::cerr << "[Device] walking pNext\n";

    while (node) {
        std::cerr << "  sType=" << node->sType << "\n";

        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(node);
            f->timelineSemaphore = VK_TRUE;
            timelineFound = true;
        }

        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* f = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(node);
            f->timelineSemaphore = VK_TRUE;
            timelineFound = true;
        }

        node = node->pNext;
    }

    std::cerr << "[Device] timeline found = " << timelineFound << "\n";

    finish();
    std::cerr << "[Device] exit\n";
}

void Root::modifySwapchainCreateInfo(
    const vk::Vulkan& vk,
    VkSwapchainCreateInfoKHR& createInfo,
    const std::function<void(void)>& finish
) const {
    std::cerr << "\n[Swapchain] enter\n";

    if (!this->active_profile.has_value()) {
        std::cerr << "[Swapchain] no profile\n";
        finish();
        return;
    }

    VkSurfaceCapabilitiesKHR caps{};
    auto res = vk.fi().GetPhysicalDeviceSurfaceCapabilitiesKHR(
        vk.physdev(),
        createInfo.surface,
        &caps
    );

    std::cerr << "[Swapchain] vkGetSurfaceCaps = " << res << "\n";

    if (res != VK_SUCCESS) {
        finish();
        return;
    }

    dump_surface_caps(caps);

    context_ModifySwapchainCreateInfo(
        *this->active_profile,
        caps.maxImageCount,
        createInfo
    );

    finish();
    std::cerr << "[Swapchain] exit\n";
}

void Root::createSwapchainContext(
    const vk::Vulkan& vk,
    VkSwapchainKHR swapchain,
    const SwapchainInfo& info
) {
    std::cerr << "\n[SwapchainCtx] enter\n";

    if (!this->active_profile.has_value()) {
        std::cerr << "[SwapchainCtx] inactive\n";
        throw ls::error("inactive layer");
    }

    if (!this->backend.has_value()) {
        std::cerr << "[SwapchainCtx] creating backend\n";

        const auto& global = this->config.get().global();

        setenv("DISABLE_LSFGVK", "1", 1);

        std::string dll =
            global.dll.has_value()
                ? *global.dll
                : ls::findShaderDll().string();

        std::cerr << "[SwapchainCtx] dll = " << dll << "\n";

        this->backend.emplace(
            [gpu = this->active_profile->gpu](
                const std::string& name,
                std::pair<const std::string&, const std::string&> ids,
                const std::optional<std::string>& pci
            ) {
                std::cerr << "[Backend probe] " << name << "\n";

                if (!gpu)
                    return true;

                return (name == *gpu) ||
                       (ids.first + ":" + ids.second == *gpu) ||
                       (pci && *pci == *gpu);
            },
            dll,
            global.allow_fp16
        );

        unsetenv("DISABLE_LSFGVK");
    }

    std::cerr << "[SwapchainCtx] emplace swapchain\n";

    this->swapchains.emplace(
        swapchain,
        Swapchain(vk, this->backend.mut(), *this->active_profile, info)
    );

    std::cerr << "[SwapchainCtx] exit\n";
}

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    std::cerr << "\n[SwapchainCtx] remove\n";
    this->swapchains.erase(swapchain);
}
