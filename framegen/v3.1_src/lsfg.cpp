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
#include <unistd.h>

using namespace LSFG;
using namespace LSFG_3_1;

/*
 * IMPORTANT:
 *
 * Do NOT use anonymous namespace statics here.
 *
 * Wine + Termux + Vulkan implicit layer loading
 * may duplicate translation-unit local statics.
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
        << "[3_1] initialize pid="
        << getpid()
        << " addr="
        << &g_instance
        << std::endl;

    /*
     * safer check
     */
    if (g_instance.has_value() &&
        g_device.has_value()) {

        std::cerr
            << "[3_1] already initialized"
            << std::endl;

        return;
    }

    /*
     * recover half-init state
     */
    if (g_instance.has_value() !=
        g_device.has_value()) {

        std::cerr
            << "[3_1] recovering half-init state"
            << std::endl;

        g_device.reset();
        g_instance.reset();
    }

    g_instance.emplace();

    g_device.emplace(Vulkan {
        .device{*g_instance, deviceUUID, forceDisableFp16},
        .generationCount = generationCount,
        .flowScale = flowScale,
        .isHdr = isHdr
    });

    g_contexts.clear();

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
        << "[3_1] initialize complete"
        << std::endl;
}

#ifdef LSFGVK_EXCESS_DEBUG
void LSFG_3_1::initializeRenderDoc() {

    if (g_renderdoc.has_value())
        return;

    if (void* mod =
            dlopen(
                "librenderdoc.so",
                RTLD_NOW | RTLD_NOLOAD)) {

        auto rdocGetAPI =
            reinterpret_cast<pRENDERDOC_GetAPI>(
                dlsym(
                    mod,
                    "RENDERDOC_GetAPI"));

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
        << "[3_1] createContext pid="
        << getpid()
        << " addr="
        << &g_instance
        << std::endl;

    if (!g_instance.has_value() ||
        !g_device.has_value()) {

        std::cerr
            << "[3_1] createContext failed"
            << std::endl;

        std::cerr
            << "[3_1] g_instance="
            << g_instance.has_value()
            << std::endl;

        std::cerr
            << "[3_1] g_device="
            << g_device.has_value()
            << std::endl;

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
        << "[3_1] context created id="
        << id
        << std::endl;

    return id;
}

void LSFG_3_1::presentContext(
        int32_t id,
        int inSem,
        const std::vector<int>& outSem) {

    std::cerr
        << "[3_1] presentContext pid="
        << getpid()
        << " addr="
        << &g_instance
        << std::endl;

    if (!g_instance.has_value() ||
        !g_device.has_value()) {

        std::cerr
            << "[3_1] presentContext failed"
            << std::endl;

        std::cerr
            << "[3_1] g_instance="
            << g_instance.has_value()
            << std::endl;

        std::cerr
            << "[3_1] g_device="
            << g_device.has_value()
            << std::endl;

        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "LSFG not initialized");
    }

    auto it = g_contexts.find(id);

    if (it == g_contexts.end()) {

        std::cerr
            << "[3_1] context missing id="
            << id
            << std::endl;

        std::cerr
            << "[3_1] total contexts="
            << g_contexts.size()
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

    std::cerr
        << "[3_1] deleteContext id="
        << id
        << std::endl;

    if (!g_instance.has_value() ||
    !g_device.has_value()) {

    std::cerr
        << "[3_1] deleteContext skipped (already finalized)"
        << std::endl;

    return;
    }

    auto it = g_contexts.find(id);

    if (it == g_contexts.end()) {

    std::cerr
        << "[3_1] deleteContext skipped (missing context)"
        << std::endl;

    return;
    }
        
    vkDeviceWaitIdle(
        g_device->device.handle());

    g_contexts.erase(it);
}

void LSFG_3_1::finalize() {

    std::cerr
        << "[3_1] finalize"
        << std::endl;

    if (!g_instance.has_value() ||
        !g_device.has_value()) {

        std::cerr
            << "[3_1] already finalized"
            << std::endl;

        return;
    }

    vkDeviceWaitIdle(
        g_device->device.handle());

    g_contexts.clear();

    g_device.reset();
    g_instance.reset();

    std::cerr
        << "[3_1] finalize complete"
        << std::endl;
}
