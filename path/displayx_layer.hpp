#pragma once

// 1. 시스템 헤더 및 X11 관련 헤더를 반드시 Vulkan보다 먼저 include
#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>

// 2. Vulkan 관련 헤더 (순서 및 중복 정리)
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_xlib.h>

// 3. 표준 라이브러리 및 프로젝트 의존성
#include <unistd.h>
#include <unordered_map>
#include <android/hardware_buffer.h>
#include <mutex>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <memory>
#include <cstring>
#include "logger.hpp"
#include "id.hpp"

// [수정] 링키지 오류 방지 및 심볼 가시성 확보
#undef VK_LAYER_EXPORT
#if defined(WIN32)
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define VK_LAYER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
#define ICD_LOADER_MAGIC 0x01D01010

template <typename T>
void* GetKey(T item) {
    return *(void**) item;
}

#define VK_WRAP_NON_DISPATCHABLE_HANDLE(type, obj) ((type)(uintptr_t)(obj))
#define VK_UNWRAP_NON_DISPATCHABLE_HANDLE(handle, type, variable) \
	type *variable = (type *)(uintptr_t)handle;

// 전역 상태 관리 (extern 선언)
extern std::unordered_map<void *, VkLayerInstanceDispatchTable> instanceDispatch;
extern std::unordered_map<void *, VkInstance> instanceMap;
extern std::unordered_map<void *, std::shared_ptr<struct device>> deviceDispatch;                             
extern std::unordered_map<VkQueue, std::shared_ptr<struct queue>> queues;
extern ID id;
extern std::mutex global_lock;

typedef std::lock_guard<std::mutex> scoped_lock;

#define GETPROCADDR(func) \
if (!strcmp(pName, "vk" #func)) \
    return (PFN_vkVoidFunction)&DisplayX_##func;

// 구조체 정의
struct fence {
	VkFence handle;
	int sync_fd;
};

struct queue {
	std::shared_ptr<struct device> device;
	VkQueue handle;
	std::unique_ptr<struct fence> fence;
};

struct device {
	VkPhysicalDevice physical;
	VkDevice handle;
	VkLayerDispatchTable table;
	std::shared_ptr<struct queue> queue;
};

struct fake_surface {
    uintptr_t loader_magic;
    VkObjectType obj_type;
    VkInstance instance;
    int native_renderer_fd;
    xcb_connection_t *conn;
    xcb_window_t window;
};

struct fake_swapchain_image {
	AHardwareBuffer *ahb;
	VkDeviceMemory memory;
	uint32_t width;
	uint32_t height;
	VkImage handle;
};

struct fake_swapchain {
    uintptr_t loader_magic;
    VkObjectType obj_type;
    VkDevice wsi_device;
    bool wait_for_present;
    bool use_prime_blit;
    struct fake_surface *surface;
    std::shared_ptr<struct device> device;
    uint32_t imageCount;
    uint32_t requestedImageCount;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR presentMode;
    std::vector<std::shared_ptr<struct fake_swapchain_image>> images;
    uint32_t currentImage;
    uint8_t id;
    std::mutex socket_mutex; 
};

// API 함수 선언 (매크로 사용)
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_CreateXcbSurfaceKHR(VkInstance, const VkXcbSurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_CreateXlibSurfaceKHR(VkInstance, const VkXlibSurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
VK_LAYER_EXPORT void VKAPI_CALL DisplayX_DestroySurfaceKHR(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*);
