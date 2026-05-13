/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain.hpp"
#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <utility>
#include <vector>
#include <iostream>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;

namespace {

#define LOG(tag, msg) \
    std::cerr << "[LSFG][" << tag << "] " << msg << std::endl

#define TRACE_FUNC() \
    std::cerr << "[LSFG][TRACE] " \
              << __FUNCTION__ \
              << " line=" \
              << __LINE__ \
              << std::endl

VkImageMemoryBarrier barrierHelper(
        VkImage handle,
        VkAccessFlags srcAccessMask,
        VkAccessFlags dstAccessMask,
        VkImageLayout oldLayout,
        VkImageLayout newLayout)
{
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = srcAccessMask,
        .dstAccessMask = dstAccessMask,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = handle,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
}

} // namespace

void layer::context_ModifySwapchainCreateInfo(
        const ls::GameConf& profile,
        uint32_t maxImages,
        VkSwapchainCreateInfoKHR& createInfo)
{
    LOG("Swapchain", "context_ModifySwapchainCreateInfo");

    createInfo.imageUsage |=
        VK_IMAGE_USAGE_TRANSFER_DST_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    switch (profile.pacing) {

        case ls::Pacing::None:

            createInfo.minImageCount +=
                profile.multiplier;

            if (maxImages
                && createInfo.minImageCount > maxImages)
            {
                createInfo.minImageCount =
                    maxImages;
            }

            createInfo.presentMode =
                VK_PRESENT_MODE_FIFO_KHR;

            break;
    }
}

Swapchain::Swapchain(
        const vk::Vulkan& vk,
        backend::Instance& backend,
        ls::GameConf profile,
        SwapchainInfo info)
    :
        instance(backend),
        profile(std::move(profile)),
        info(std::move(info))
{
    TRACE_FUNC();

    LOG("Ctor", "Swapchain ctor enter");

    const VkExtent2D extent =
        this->info.extent;

    const bool hdr =
        this->info.format > 57;

    LOG("Ctor",
        "extent="
        << extent.width
        << "x"
        << extent.height);

    LOG("Ctor",
        "hdr="
        << hdr);

    std::vector<int> sourceFds(2);

    std::vector<int> destinationFds(
        this->profile.multiplier - 1);

    LOG("Ctor",
        "source fd count="
        << sourceFds.size());

    LOG("Ctor",
        "destination fd count="
        << destinationFds.size());

    this->sourceImages.reserve(
        sourceFds.size());

    for (int& fd : sourceFds) {

        LOG("Ctor",
            "creating source image");

        this->sourceImages.emplace_back(
            vk,
            extent,
            hdr
                ? VK_FORMAT_R16G16B16A16_SFLOAT
                : VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT
                | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt,
            &fd);

        LOG("Ctor",
            "source image fd="
            << fd);
    }

    this->destinationImages.reserve(
        destinationFds.size());

    for (int& fd : destinationFds) {

        LOG("Ctor",
            "creating destination image");

        this->destinationImages.emplace_back(
            vk,
            extent,
            hdr
                ? VK_FORMAT_R16G16B16A16_SFLOAT
                : VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt,
            &fd);

        LOG("Ctor",
            "destination image fd="
            << fd);
    }

    int syncFd{};

    LOG("Ctor",
        "creating sync semaphore");

    this->syncSemaphore.emplace(
        vk,
        0,
        std::nullopt,
        &syncFd);

    LOG("Ctor",
        "sync fd="
        << syncFd);

    try {

        LOG("Ctor",
            "opening backend context");

        this->ctx =
            ls::owned_ptr<
                ls::R<backend::Context>>(
                new ls::R<backend::Context>(
                    backend.openContext(
                        {
                            sourceFds.at(0),
                            sourceFds.at(1)
                        },
                        destinationFds,
                        syncFd,
                        extent.width,
                        extent.height,
                        hdr,
                        1.0F
                            / this->profile.flow_scale,
                        this->profile.performance_mode
                    )),
                [backend = &backend](
                        ls::R<backend::Context>& ctx)
                {
                    backend->closeContext(ctx);
                });

        LOG("Ctor",
            "backend context opened");

        backend::makeLeaking();

        LOG("Ctor",
            "backend leaking enabled");
    }
    catch (const std::exception& e) {

        LOG("Ctor",
            "backend open failed: "
            << e.what());

        throw ls::error(
            "failed to create swapchain context",
            e);
    }

    LOG("Ctor",
        "creating render command buffer");

    this->renderCommandBuffer.emplace(vk);

    LOG("Ctor",
        "creating render fence");

    this->renderFence.emplace(vk);

    for (size_t i = 0;
         i < this->destinationImages.size();
         i++)
    {
        LOG("Ctor",
            "creating render pass "
            << i);

        this->passes.emplace_back(
            RenderPass {
                .commandBuffer =
                    vk::CommandBuffer(vk),
                .acquireSemaphore =
                    vk::Semaphore(vk)
            });
    }

    const size_t frames =
        std::max(
            this->info.images.size(),
            this->destinationImages.size() + 2);

    LOG("Ctor",
        "frames="
        << frames);

    for (size_t i = 0;
         i < frames;
         i++)
    {
        LOG("Ctor",
            "creating post semaphore pair "
            << i);

        this->postCopySemaphores.emplace_back(
            vk::Semaphore(vk),
            vk::Semaphore(vk)
        );
    }

    LOG("Ctor",
        "Swapchain ctor exit");
}

VkResult Swapchain::present(
        const vk::Vulkan& vk,
        VkQueue queue,
        VkSwapchainKHR swapchain,
        void* next_chain,
        uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores)
{
    TRACE_FUNC();

    LOG("Present", "ENTER");

    LOG("Present",
        "imageIdx="
        << imageIdx);

    LOG("Present",
        "fidx="
        << this->fidx);

    LOG("Present",
        "idx="
        << this->idx);

    const auto& swapchainImage =
        this->info.images.at(imageIdx);

    LOG("Present",
        "swapchain image ready");

    const auto& sourceImage =
        this->sourceImages.at(
            this->fidx % 2);

    LOG("Present",
        "source image selected");

    try {

        LOG("Present",
            "scheduleFrames begin");

        this->instance.get().scheduleFrames(
            this->ctx.get());

        LOG("Present",
            "scheduleFrames done");
    }
    catch (const std::exception& e) {

        LOG("Present",
            "scheduleFrames exception: "
            << e.what());

        throw ls::error(
            "failed to schedule frames",
            e);
    }

    if (this->profile.pacing
        == ls::Pacing::None)
    {
#if defined(VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) && defined(VkSwapchainPresentModeInfoEXT)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

        LOG("Present",
            "patching present mode");

        auto* info =
            reinterpret_cast<
                VkSwapchainPresentModeInfoEXT*>(
                    next_chain);

        while (info) {

            LOG("Present",
                "next_chain sType="
                << info->sType);

            if (info->sType
                == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT)
            {
                for (size_t i = 0;
                     i < info->swapchainCount;
                     i++)
                {
                    const_cast<VkPresentModeKHR*>(
                        info->pPresentModes)[i] =
                            VK_PRESENT_MODE_FIFO_KHR;
                }
            }

            info =
                reinterpret_cast<
                    VkSwapchainPresentModeInfoEXT*>(
                        const_cast<void*>(
                            info->pNext));
        }

#pragma clang diagnostic pop

#endif
    }

    LOG("Present",
        "waiting render fence");

    if (this->fidx
        && !this->renderFence->wait(
            vk,
            150ULL * 1000 * 1000))
    {
        LOG("Present",
            "render fence timeout");

        throw ls::vulkan_error(
            VK_TIMEOUT,
            "vkWaitForFences() failed");
    }

    LOG("Present",
        "render fence wait done");

    this->renderFence->reset(vk);

    LOG("Present",
        "render fence reset");

    const auto& cmdbuf =
        *this->renderCommandBuffer;

    LOG("Present",
        "main cmdbuf begin");

    cmdbuf.begin(vk);

    LOG("Present",
        "main blit begin");

    cmdbuf.blitImage(
        vk,
        {
            barrierHelper(
                swapchainImage,
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ),
            barrierHelper(
                sourceImage.handle(),
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            ),
        },
        {
            swapchainImage,
            sourceImage.handle()
        },
        sourceImage.getExtent(),
        {
            barrierHelper(
                swapchainImage,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            ),
        }
    );

    LOG("Present",
        "main blit done");

    cmdbuf.end(vk);

    LOG("Present",
        "main submit begin");

    cmdbuf.submit(
        vk,
        semaphores,
        VK_NULL_HANDLE,
        0,
        {},
        this->syncSemaphore->handle(),
        this->idx++
    );

    LOG("Present",
        "main submit done");

    for (size_t i = 0;
         i < this->destinationImages.size();
         i++)
    {
        LOG("Present",
            "destination pass="
            << i);

        auto& pcs =
            this->postCopySemaphores.at(
                this->idx
                % this->postCopySemaphores.size());

        auto& destinationImage =
            this->destinationImages.at(i);

        auto& pass =
            this->passes.at(i);

        uint32_t aqImageIdx{};

        LOG("Present",
            "AcquireNextImageKHR begin");

        auto res =
            vk.df().AcquireNextImageKHR(
                vk.dev(),
                swapchain,
                UINT64_MAX,
                pass.acquireSemaphore.handle(),
                VK_NULL_HANDLE,
                &aqImageIdx
            );

        LOG("Present",
            "AcquireNextImageKHR result="
            << res);

        if (res != VK_SUCCESS
            && res != VK_SUBOPTIMAL_KHR)
        {
            throw ls::vulkan_error(
                res,
                "vkAcquireNextImageKHR() failed");
        }

        const auto& aquiredSwapchainImage =
            this->info.images.at(
                aqImageIdx);

        auto& cmdbuf =
            pass.commandBuffer;

        LOG("Present",
            "pass cmdbuf begin");

        cmdbuf.begin(vk);

        LOG("Present",
            "destination blit begin");

        cmdbuf.blitImage(
            vk,
            {
                barrierHelper(
                    destinationImage.handle(),
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                ),
                barrierHelper(
                    aquiredSwapchainImage,
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                ),
            },
            {
                destinationImage.handle(),
                aquiredSwapchainImage
            },
            destinationImage.getExtent(),
            {
                barrierHelper(
                    aquiredSwapchainImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                ),
            }
        );

        LOG("Present",
            "destination blit done");

        std::vector<VkSemaphore> waitSemaphores{
            pass.acquireSemaphore.handle()
        };

        if (i) {

            const auto& prevPCS =
                this->postCopySemaphores.at(
                    (this->idx - 1)
                    % this->postCopySemaphores.size());

            waitSemaphores.push_back(
                prevPCS.second.handle());
        }

        const std::vector<VkSemaphore>
            signalSemaphores{
                pcs.first.handle(),
                pcs.second.handle()
            };

        cmdbuf.end(vk);

        LOG("Present",
            "pass submit begin");

        cmdbuf.submit(
            vk,
            waitSemaphores,
            this->syncSemaphore->handle(),
            this->idx,
            signalSemaphores,
            VK_NULL_HANDLE,
            0,
            i == this->destinationImages.size() - 1
                ? this->renderFence->handle()
                : VK_NULL_HANDLE
        );

        LOG("Present",
            "pass submit done");

        const VkPresentInfoKHR presentInfo{
            .sType =
                VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext =
                i ? nullptr : next_chain,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores =
                &pcs.first.handle(),
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &aqImageIdx,
        };

        LOG("Present",
            "QueuePresentKHR begin");

        res =
            vk.df().QueuePresentKHR(
                queue,
                &presentInfo);

        LOG("Present",
            "QueuePresentKHR result="
            << res);

        if (res != VK_SUCCESS
            && res != VK_SUBOPTIMAL_KHR)
        {
            throw ls::vulkan_error(
                res,
                "vkQueuePresentKHR() failed");
        }

        this->idx++;
    }

    LOG("Present",
        "final present begin");

    auto& lastPCS =
        this->postCopySemaphores.at(
            (this->idx - 1)
            % this->postCopySemaphores.size());

    const VkPresentInfoKHR presentInfo{
        .sType =
            VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores =
            &lastPCS.second.handle(),
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIdx,
    };

    auto res =
        vk.df().QueuePresentKHR(
            queue,
            &presentInfo);

    LOG("Present",
        "final present result="
        << res);

    if (res != VK_SUCCESS
        && res != VK_SUBOPTIMAL_KHR)
    {
        throw ls::vulkan_error(
            res,
            "vkQueuePresentKHR() failed");
    }

    this->fidx++;

    LOG("Present",
        "EXIT");

    return res;
}
