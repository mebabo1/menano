#include <iostream>
#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "lsfg_3_1.hpp"
#include "v3_1/context.hpp"
#include "core/commandpool.hpp"
#include "core/descriptorpool.hpp"
#include "core/instance.hpp"
#include "pool/shaderpool.hpp"
#include "common/exception.hpp"
#include "common/utils.hpp"

#ifdef LSFGVK_EXCESS_DEBUG
#include <renderdoc_app.h>
#include <dlfcn.h>
#endif // LSFGVK_EXCESS_DEBUG

#include <cstdint>
#include <optional>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace LSFG;
using namespace LSFG_3_1;

/*
 * IMPORTANT:
 *
 * Do NOT use anonymous namespace statics here.
 *
 * Wine + Termux + Vulkan implicit layer loading
 * can duplicate translation-unit local statics.
 *
 * Use global statics instead.
 */

static std::optional<Core::Instance> g_instance;
static std::optional<Vulkan> g_device;
static std::unordered_map<int32_t, Context> g_contexts;

#ifdef LSFGVK_EXCESS_DEBUG
static std::optional<RENDERDOC_API_1_6_0*> g_renderdoc;
#endif // LSFGVK_EXCESS_DEBUG

void LSFG_3_1::initialize(
        uint64_t deviceUUID,
        bool isHdr,
        float flowScale,
        uint64_t generationCount,
        bool forceDisableFp16,
        const std::function<std::vector<uint8_t>(
            const std::string&, bool)>& loader) {

    std::cerr
        << "initialize instance addr = "
        << &g_instance
        << std::endl;

    if (g_instance.has_value() || g_device.has_value()) {
        std::cerr
            << "LSFG already initialized"
            << std::endl;
        return;
    }

    g_instance.emplace();

    g_device.emplace(Vulkan {
        .device{*g_instance, deviceUUID, forceDisableFp16},
        .generationCount = generationCount,
        .flowScale = flowScale,
        .isHdr = isHdr
    });

    g_contexts = std::unordered_map<int32_t, Context>();

    g_device->commandPool =
        Core::CommandPool(g_device->device);

    g_device->descriptorPool =
        Core::DescriptorPool(g_device->device);

    g_device->resources =
        Pool::ResourcePool(
            g_device->isHdr,
            g_device->flowScale);

    g_device->shaders =
        Pool::ShaderPool(
            loader,
            g_device->device.getFP16Support());

    std::srand(
        static_cast<uint32_t>(
            std::time(nullptr)));

    std::cerr
        << "LSFG initialize complete"
        << std::endl;
}

#ifdef LSFGVK_EXCESS_DEBUG
void LSFG_3_1::initializeRenderDoc() {
    if (g_renderdoc.has_value())
        return;

    if (void* mod =
            dlopen("librenderdoc.so",
                   RTLD_NOW | RTLD_NOLOAD)) {

        auto rdocGetAPI =
            reinterpret_cast<pRENDERDOC_GetAPI>(
                dlsym(mod, "RENDERDOC_GetAPI"));

        RENDERDOC_API_1_6_0* rdoc{};

        rdocGetAPI(
            eRENDERDOC_API_Version_1_6_0,
            reinterpret_cast<void**>(&rdoc));

        g_renderdoc.emplace(rdoc);
    }

    if (!g_renderdoc.has_value()) {
        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "RenderDoc API not found");
    }
}
#endif // LSFGVK_EXCESS_DEBUG

int32_t LSFG_3_1::createContext(
        int in0,
        int in1,
        const std::vector<int>& outN,
        VkExtent2D extent,
        VkFormat format) {

    std::cerr
        << "createContext instance addr = "
        << &g_instance
        << std::endl;

    if (!g_instance.has_value() ||
        !g_device.has_value()) {

        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "LSFG not initialized");
    }

    const int32_t id = std::rand();

    g_contexts.emplace(
        id,
        Context(
            *g_device,
            in0,
            in1,
            outN,
            extent,
            format));

    std::cerr
        << "LSFG context created id = "
        << id
        << std::endl;

    return id;
}

void LSFG_3_1::presentContext(
        int32_t id,
        int inSem,
        const std::vector<int>& outSem) {

    std::cerr
        << "presentContext instance addr = "
        << &g_instance
        << std::endl;

    if (!g_instance.has_value() ||
        !g_device.has_value()) {

        std::cerr
            << "g_instance.has_value() = "
            << g_instance.has_value()
            << std::endl;

        std::cerr
            << "g_device.has_value() = "
            << g_device.has_value()
            << std::endl;

        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "LSFG not initialized");
    }

    auto it = g_contexts.find(id);

    if (it == g_contexts.end()) {
        std::cerr
            << "Context not found: "
            << id
            << std::endl;

        throw LSFG::vulkan_error(
            VK_ERROR_UNKNOWN,
            "Context not found");
    }

#ifdef LSFGVK_EXCESS_DEBUG
    if (g_renderdoc.has_value()) {
        (*g_renderdoc)->StartFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(
                g_instance->handle()),
            nullptr);
    }
#endif // LSFGVK_EXCESS_DEBUG

    it->second.present(
        *g_device,
        inSem,
        outSem);

#ifdef LSFGVK_EXCESS_DEBUG
    if (g_renderdoc.has_value()) {
        vkDeviceWaitIdle(
            g_device->device.handle());

        (*g_renderdoc)->EndFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(
                g_instance->handle()),
            nullptr);
    }
#endif // LSFGVK_EXCESS_DEBUG
}

void LSFG_3_1::deleteContext(int32_t id) {
    if (!g_instance.has_value() ||
        !g_device.has_value()) {

        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "LSFG not initialized");
    }

    auto it = g_contexts.find(id);

    if (it == g_contexts.end()) {
        throw LSFG::vulkan_error(
            VK_ERROR_DEVICE_LOST,
            "No such context");
    }

    vkDeviceWaitIdle(
        g_device->device.handle());

    g_contexts.erase(it);
}

void LSFG_3_1::finalize() {
    std::cerr
        << "LSFG finalize called"
        << std::endl;

    if (!g_instance.has_value() ||
        !g_device.has_value()) {
        return;
    }

    vkDeviceWaitIdle(
        g_device->device.handle());

    g_contexts.clear();

    g_device.reset();
    g_instance.reset();
}
