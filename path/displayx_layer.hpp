#pragma once

#include "vulkan/vk_layer.h"
#include "logger.hpp"
#include "id.hpp"

#include <vulkan/vulkan.h>
#include <unistd.h>
#include <unordered_map>
#include <android/hardware_buffer.h>
#include <mutex>
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <libsync.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_xlib.h>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <memory>
#include <cstring>

#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
#define ICD_LOADER_MAGIC 0x01D01010

template <typename T>
void* GetKey(T item) {
    return *(void**) item;
}

#define VK_WRAP_NON_DISPATCHABLE_HANDLE(type, obj) ((type)(uintptr_t)(obj))
#define VK_UNWRAP_NON_DISPATCHABLE_HANDLE(handle, type, variable) \
	type *variable = (type *)(uintptr_t)handle;

// [링킹 에러 방지 수정] 중복 정의 에러를 막기 위해 extern 선언으로 스타일을 정돈합니다.
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
    
    // [데드락 방지 수정] 재생성 시 큐 싱크를 위한 카운터를 구조체 내부에 안전하게 포함합니다.
    uint32_t frame_count; 
};

VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance);
VK_LAYER_EXPORT void VKAPI_CALL DisplayX_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice);
VK_LAYER_EXPORT void VKAPI_CALL DisplayX_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator);
VK_LAYER_EXPORT void VKAPI_CALL DisplayX_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue);
VK_LAYER_EXPORT void VKAPI_CALL DisplayX_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue *pQueue);

VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR *pSurface);
VK_LAYER_EXPORT void VKAPI_CALL DisplayX_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceSurfaceInfo2KHR *pInfo, VkSurfaceCapabilities2KHR *pSurfaceCapabilities);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceSurfaceInfo2KHR *pInfo, uint32_t* pSurfaceFormatCount, VkSurfaceFormat2KHR* pSurfaceFormats);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfacePresentModeCount, VkPresentModeKHR* pPresentModes);

VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);
VK_LAYER_EXPORT void VKAPI_CALL DisplayX_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t *pImageIndex);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, uint64_t flags);

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL DisplayX_GetDeviceProcAddr(VkDevice device, const char *pName);
VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL DisplayX_GetInstanceProcAddr(VkInstance instance, const char *pName);

extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName);
extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName);
