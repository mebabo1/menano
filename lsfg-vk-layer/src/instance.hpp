/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "swapchain.hpp"

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::layer {

    /// root context of the lsfg-vk layer
    class Root {
    public:

        /// create the lsfg-vk root context
        /// @throws ls::error on failure
        Root();

        /// check if the layer is active
        /// @return true if active
        [[nodiscard]]
        bool active() const {
            return this->active_profile.has_value();
        }

        /// ensure the layer is up-to-date
        /// @return true if the configuration was updated
        bool update();

        /// modify instance create info
        /// @param createInfo original create info
        /// @param finish function to call after modification
        void modifyInstanceCreateInfo(
            VkInstanceCreateInfo& createInfo,
            const std::function<void(void)>& finish) const;

        /// modify device create info
        /// @param createInfo original create info
        /// @param finish function to call after modification
        void modifyDeviceCreateInfo(
            VkDeviceCreateInfo& createInfo,
            const std::function<void(void)>& finish) const;

        /// modify swapchain create info
        /// @param vk vulkan instance
        /// @param createInfo original create info
        /// @param finish function to call after modification
        void modifySwapchainCreateInfo(
            const vk::Vulkan& vk,
            VkSwapchainCreateInfoKHR& createInfo,
            const std::function<void(void)>& finish) const;

        /// create swapchain context
        /// @param vk vulkan instance
        /// @param swapchain swapchain handle
        /// @param info swapchain info
        /// @throws ls::error on failure
        void createSwapchainContext(
            const vk::Vulkan& vk,
            VkSwapchainKHR swapchain,
            const SwapchainInfo& info);

        /// get swapchain context
        /// @param swapchain swapchain handle
        /// @return swapchain context
        /// @throws ls::error if not found
        [[nodiscard]]
        Swapchain& getSwapchainContext(
            VkSwapchainKHR swapchain)
        {
            const auto& it =
                this->swapchains.find(swapchain);

            if (it == this->swapchains.end()) {
                throw ls::error(
                    "swapchain context not found");
            }

            return it->second;
        }

        /// remove swapchain context
        /// @param swapchain swapchain handle
        void removeSwapchainContext(
            VkSwapchainKHR swapchain);

    private:

        // =====================================================
        // CONFIG
        // =====================================================

        ls::WatchedConfig config;

        std::optional<ls::GameConf>
            active_profile;

        // =====================================================
        // SAFE EXTENSION STORAGE
        //
        // prevents dangling:
        // createInfo.ppEnabledExtensionNames
        //
        // Vulkan/Wine loaders may access extension arrays
        // after callback scope exits.
        // =====================================================

        mutable std::vector<const char*>
            instanceExtensionsStorage;

        mutable std::vector<const char*>
            deviceExtensionsStorage;

        // =====================================================
        // SAFE FEATURE STORAGE
        //
        // prevents dangling pNext chains:
        // createInfo.pNext
        //
        // Winevulkan/DXVK may access pNext structures
        // after callback scope exits.
        // =====================================================

        mutable std::optional<
            VkPhysicalDeviceTimelineSemaphoreFeatures>
                timelineSemaphoreFeaturesStorage;

        // Optional future-safe storage
        // for VK_KHR_synchronization2 support.

        mutable std::optional<
            VkPhysicalDeviceSynchronization2Features>
                synchronization2FeaturesStorage;

        // =====================================================
        // BACKEND
        // =====================================================

        ls::lazy<backend::Instance>
            backend;

        // =====================================================
        // SWAPCHAINS
        // =====================================================

        std::unordered_map<
            VkSwapchainKHR,
            Swapchain>
            swapchains;
    };

}
