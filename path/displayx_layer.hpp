#pragma once

#include "vulkan/vk_layer.h"
#include "logger.hpp"
#include "id.hpp"

#include <vulkan/vulkan.h>
#include <unistd.h>
#include <unordered_map>
#include <mutex>
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_xlib.h>
#include <vector>
#include <memory>
#include <cstring>

#define ICD_LOADER_MAGIC 0x01D01010

// 🛠️ [보안 & MTE 대응 패치] 디스패처블 핸들의 내부 포인터 오염을 방지하기 위한 안전한 키 변환 함수
template <typename T>
uint64_t GetKey(T item) {
    if (!item) return 0;
    return (uint64_t)(uintptr_t)(*(void**)item);
}

// 🛠️ 안드로이드 64비트 주소 정형화를 위한 단일 변환 매크로 정의
#define SAFE_KEY(handle) ((uint64_t)(uintptr_t)(handle))

#define VK_WRAP_NON_DISPATCHABLE_HANDLE(type, obj) ((type)(uintptr_t)(obj))
#define VK_UNWRAP_NON_DISPATCHABLE_HANDLE(handle, type, variable) \
	type *variable = (type *)(uintptr_t)handle;

// 🛠️ [컴파일 에러 해결의 핵심] 전역 맵들의 Key 타입을 cpp와 동일하게 uint64_t로 단일화
extern std::unordered_map<uint64_t, VkLayerInstanceDispatchTable> instanceDispatch;
extern std::unordered_map<uint64_t, VkInstance> instanceMap;
extern std::unordered_map<uint64_t, std::shared_ptr<struct device>> deviceDispatch;                             
extern std::unordered_map<uint64_t, std::shared_ptr<struct queue>> queues;

// 🛠️ [추가] 물리 장치(VkPhysicalDevice)를 통해 소속 인스턴스(VkInstance)를 역추적하기 위한 전역 맵 선언
extern std::unordered_map<uint64_t, VkInstance> physDevToInstance; 

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

	// ⭐ [하드웨어 dma-buf FD 추출 엔진 전용 함수 포인터 보관]
	PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
	PFN_vkMapMemory MapMemory;
	PFN_vkUnmapMemory UnmapMemory;
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
    uint32_t frame_count; 
};

#ifdef __cplusplus
extern "C" {
#endif

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

// ⭐ [중요 추가] 세그폴트 방지 및 WSI Mocking용으로 소스 파일에 정의된 인터셉터들과 완벽 대응 싱크 맞춤
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats);
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

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName);
VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName);
VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo, VkSurfaceCapabilities2KHR* pSurfaceCapabilities);

#ifdef __cplusplus
}
#endif
