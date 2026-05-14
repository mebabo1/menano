/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/buffer.hpp"
#include "lsfg-vk-common/vulkan/descriptor_set.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/shader.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <iostream>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace vk;

namespace {
    /// create a command buffer
    ls::owned_ptr<VkCommandBuffer> createCommandBuffer(const vk::Vulkan& vk) {
        VkCommandBuffer handle{};

        const VkCommandBufferAllocateInfo commandBufferInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = vk.cmdpool(),
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1
        };
        auto res = vk.df().AllocateCommandBuffers(vk.dev(), &commandBufferInfo, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkAllocateCommandBuffers() failed");

        auto setLoaderData = vk.loaderdatafunc();
        if (setLoaderData) {
            res = (*setLoaderData)(vk.dev(), handle);
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "vkSetDeviceLoaderData() failed");
        }

        return ls::owned_ptr<VkCommandBuffer>(
            new VkCommandBuffer(handle),
            [dev = vk.dev(), pool = vk.cmdpool(), defunc = vk.df().FreeCommandBuffers](
                VkCommandBuffer& commandBufferModule
            ) {
                defunc(dev, pool, 1, &commandBufferModule);
            }
        );
    }
}

CommandBuffer::CommandBuffer(const vk::Vulkan& vk)
        : commandBuffer(createCommandBuffer(vk)) {}

void CommandBuffer::begin(const vk::Vulkan& vk) const {
    const VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    auto res = vk.df().BeginCommandBuffer(*this->commandBuffer, &beginInfo);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkBeginCommandBuffer() failed");
}

void CommandBuffer::insertBarriers(const vk::Vulkan& vk,
        const std::vector<vk::Barrier>& barriers) const {
    vk.df().CmdPipelineBarrier(*this->commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0, VK_NULL_HANDLE,
        0, VK_NULL_HANDLE,
        static_cast<uint32_t>(barriers.size()), barriers.data()
    );
}

void CommandBuffer::dispatch(const vk::Vulkan& vk,
        const vk::Shader& shader,
        const vk::DescriptorSet& set,
        const std::vector<vk::Barrier>& barriers,
        uint32_t x, uint32_t y, uint32_t z) const {
    vk.df().CmdPipelineBarrier(*this->commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, VK_NULL_HANDLE,
        0, VK_NULL_HANDLE,
        static_cast<uint32_t>(barriers.size()), barriers.data()
    );
    vk.df().CmdBindPipeline(*this->commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        shader.pipeline()
    );
    vk.df().CmdBindDescriptorSets(*this->commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        shader.pipelinelayout(),
        0, 1, &set.handle(),
        0, VK_NULL_HANDLE
    );
    vk.df().CmdDispatch(*this->commandBuffer, x, y, z);
}

void CommandBuffer::blitImage(const vk::Vulkan& vk,
        const std::vector<vk::Barrier>& preBarriers,
        std::pair<VkImage, VkImage> images, VkExtent2D extent,
        const std::vector<vk::Barrier>& postBarriers) const {
    vk.df().CmdPipelineBarrier(*this->commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0, VK_NULL_HANDLE,
        0, VK_NULL_HANDLE,
        static_cast<uint32_t>(preBarriers.size()), preBarriers.data()
    );

    const VkImageBlit region{
        .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1
        },
        .srcOffsets = {
            { 0, 0, 0 },
            { static_cast<int32_t>(extent.width),
              static_cast<int32_t>(extent.height), 1 }
        },
        .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1
        },
        .dstOffsets = {
            { 0, 0, 0 },
            { static_cast<int32_t>(extent.width),
              static_cast<int32_t>(extent.height), 1 }
        }
    };
    vk.df().CmdBlitImage(*this->commandBuffer,
        images.first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        images.second, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region,
        VK_FILTER_NEAREST
    );

    vk.df().CmdPipelineBarrier(*this->commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0, VK_NULL_HANDLE,
        0, VK_NULL_HANDLE,
        static_cast<uint32_t>(postBarriers.size()), postBarriers.data()
    );
}

void CommandBuffer::copyBufferToImage(const vk::Vulkan& vk,
        const vk::Buffer& buffer, const vk::Image& image) const {
    const VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_NONE,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1
        }
    };
    vk.df().CmdPipelineBarrier(*this->commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0, VK_NULL_HANDLE,
        0, VK_NULL_HANDLE,
        1, &barrier
    );

    const VkBufferImageCopy region{
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1
        },
        .imageExtent = {
            .width = image.getExtent().width,
            .height = image.getExtent().height,
            .depth = 1
        }
    };
    vk.df().CmdCopyBufferToImage(*this->commandBuffer,
        buffer.handle(), image.handle(),
        VK_IMAGE_LAYOUT_GENERAL, 1, &region
    );
}

void CommandBuffer::end(const vk::Vulkan& vk) const {
    auto res = vk.df().EndCommandBuffer(*this->commandBuffer);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkEndCommandBuffer() failed");
}

void CommandBuffer::submit(
        const vk::Vulkan& vk,
        std::vector<VkSemaphore> waitSemaphores,
        VkSemaphore waitTimelineSemaphore,
        uint64_t waitValue,
        std::vector<VkSemaphore> signalSemaphores,
        VkSemaphore signalTimelineSemaphore,
        uint64_t signalValue,
        VkFence fence) const
{
    // =====================================================
    // TIMELINE SEMAPHORE SETUP
    // =====================================================

    bool hasTimelineWait =
        waitTimelineSemaphore != VK_NULL_HANDLE;

    bool hasTimelineSignal =
        signalTimelineSemaphore != VK_NULL_HANDLE;

    if (hasTimelineWait) {
        waitSemaphores.push_back(
            waitTimelineSemaphore);
    }

    if (hasTimelineSignal) {
        signalSemaphores.push_back(
            signalTimelineSemaphore);
    }

    // =====================================================
    // TIMELINE VALUES
    // =====================================================

    std::vector<uint64_t> waitValues(
        waitSemaphores.size(),
        0);

    std::vector<uint64_t> signalValues(
        signalSemaphores.size(),
        0);

    if (hasTimelineWait && !waitValues.empty()) {
        waitValues.back() = waitValue;
    }

    if (hasTimelineSignal && !signalValues.empty()) {
        signalValues.back() = signalValue;
    }

    // =====================================================
    // PIPELINE STAGES
    // =====================================================

    std::vector<VkPipelineStageFlags> stages(
        waitSemaphores.size(),
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    // =====================================================
    // OPTIONAL TIMELINE INFO
    // =====================================================

    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    const void* next = nullptr;

    if (hasTimelineWait || hasTimelineSignal) {

        timelineInfo =
            VkTimelineSemaphoreSubmitInfo{
                .sType =
                    VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,

                .pNext = nullptr,

                .waitSemaphoreValueCount =
                    static_cast<uint32_t>(
                        waitValues.size()),

                .pWaitSemaphoreValues =
                    waitValues.empty()
                        ? nullptr
                        : waitValues.data(),

                .signalSemaphoreValueCount =
                    static_cast<uint32_t>(
                        signalValues.size()),

                .pSignalSemaphoreValues =
                    signalValues.empty()
                        ? nullptr
                        : signalValues.data()
            };

        next = &timelineInfo;
    }

    // =====================================================
    // SUBMIT INFO
    // =====================================================

    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,

        .pNext = next,

        .waitSemaphoreCount =
            static_cast<uint32_t>(
                waitSemaphores.size()),

        .pWaitSemaphores =
            waitSemaphores.empty()
                ? nullptr
                : waitSemaphores.data(),

        .pWaitDstStageMask =
            stages.empty()
                ? nullptr
                : stages.data(),

        .commandBufferCount = 1,

        .pCommandBuffers =
            &*this->commandBuffer,

        .signalSemaphoreCount =
            static_cast<uint32_t>(
                signalSemaphores.size()),

        .pSignalSemaphores =
            signalSemaphores.empty()
                ? nullptr
                : signalSemaphores.data()
    };

    // =====================================================
    // DEBUG LOGGING
    // =====================================================

    std::cerr
        << "[LSFG][Submit] "
        << "waitCount="
        << waitSemaphores.size()
        << " signalCount="
        << signalSemaphores.size()
        << " waitTimeline="
        << hasTimelineWait
        << " signalTimeline="
        << hasTimelineSignal
        << std::endl;

    // =====================================================
    // SUBMIT
    // =====================================================

    auto res =
        vk.df().QueueSubmit(
            vk.queue(),
            1,
            &submitInfo,
            fence);

    if (res != VK_SUCCESS) {

        std::cerr
            << "[LSFG][Submit] "
            << "QueueSubmit failed: "
            << res
            << std::endl;

        throw ls::vulkan_error(
            res,
            "vkQueueSubmit() failed");
    }
}
