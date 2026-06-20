#include "displayx_layer.hpp"
#include <vector>
#include <unistd.h>
#include <algorithm>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <memory>

#define GETPROCADDR(func) \
if (!strcmp(pName, "vk" #func)) \
    return (PFN_vkVoidFunction)&DisplayX_##func;

// 글로벌 상태 관리를 위한 스토리지
std::unordered_map<uint64_t, VkLayerInstanceDispatchTable> instanceDispatch;
std::unordered_map<uint64_t, VkInstance> instanceMap;
std::unordered_map<uint64_t, std::shared_ptr<struct device>> deviceDispatch;                             
std::unordered_map<uint64_t, std::shared_ptr<struct queue>> queues;

// 물리 장치 핸들이 어떤 인스턴스에 속해 있는지 추적하기 위한 전역 매핑
std::unordered_map<uint64_t, VkInstance> physDevToInstance; 

std::mutex global_lock;

struct PresentationPacket {
    uint32_t window_id;
    uint32_t image_index;
    uint32_t width;
    uint32_t height;
    uint32_t format;
};

// [Utility Functions]
int pick_memory_index(VkInstance instance, VkPhysicalDevice physical, uint32_t memoryBits) {
    VkPhysicalDeviceMemoryProperties memoryProps{};
    uint32_t idx;
    
    std::lock_guard<std::mutex> l(global_lock);
    // 🔑 디스패치 테이블 조회는 GetKey를 사용합니다.
    auto it = instanceDispatch.find(GetKey(instance)); 
    if (it != instanceDispatch.end()) {
        it->second.GetPhysicalDeviceMemoryProperties(physical, &memoryProps);
        for (idx = 0; idx < memoryProps.memoryTypeCount; idx++) {
            if (memoryBits & (1u << idx)) return idx;
        }
    }
    return UINT32_MAX;
}

static int send_dma_buf_to_x11_app(int window_id, int img_idx, int fd_to_send, uint32_t w, uint32_t h, uint32_t fmt) {
    const char* socket_path = "/data/data/com.termux/files/usr/tmp/displayx_compositor.sock";
    
    int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(sock);
        return -2; 
    }

    PresentationPacket packet{ (uint32_t)window_id, (uint32_t)img_idx, w, h, fmt };

    struct msghdr msg{};
    struct iovec iov[1];
    iov[0].iov_base = &packet;
    iov[0].iov_len = sizeof(packet);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    char cmsghdbuf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsghdbuf;
    msg.msg_controllen = sizeof(cmsghdbuf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    ::memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    ::sendmsg(sock, &msg, 0);
    ::close(sock);
    return 0;
}

// --- [Vulkan Core Intercepts] ---

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, uint64_t flags)
{
    return VK_SUCCESS; 
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
{
    pSurfaceCapabilities->minImageCount = 2;
    pSurfaceCapabilities->maxImageCount = 8;
    pSurfaceCapabilities->currentExtent = {0xFFFFFFFF, 0xFFFFFFFF}; 
    pSurfaceCapabilities->minImageExtent = {1, 1};
    pSurfaceCapabilities->maxImageExtent = {8112, 8112};
    pSurfaceCapabilities->maxImageArrayLayers = 1;
    pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    pSurfaceCapabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats)
{
    if (pSurfaceFormats == nullptr) {
        *pSurfaceFormatCount = 1;
        return VK_SUCCESS;
    }
    pSurfaceFormats[0].format = VK_FORMAT_B8G8R8A8_UNORM; 
    pSurfaceFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    *pSurfaceFormatCount = 1;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes)
{
    if (pPresentModes == nullptr) {
        *pPresentModeCount = 1;
        return VK_SUCCESS;
    }
    pPresentModes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *pPresentModeCount = 1;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{	
    VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;
    VkResult result;
    VkInstanceCreateInfo createInfo = *pCreateInfo;
	
    while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
        layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
    }
    if (!layerCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    
    PFN_vkGetInstanceProcAddr gip = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    std::vector<const char *> enabledExtensions;
    if (!createInfo.ppEnabledExtensionNames) {
        enabledExtensions.reserve(2);
    } else {
        enabledExtensions.reserve(createInfo.enabledExtensionCount + 2);
        enabledExtensions.insert(enabledExtensions.end(), createInfo.ppEnabledExtensionNames, createInfo.ppEnabledExtensionNames + createInfo.enabledExtensionCount);
    }
    enabledExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
    enabledExtensions.push_back(VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME);
    createInfo.enabledExtensionCount = enabledExtensions.size();
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();

    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gip(VK_NULL_HANDLE, "vkCreateInstance");
    result = createInstance(&createInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    VkLayerInstanceDispatchTable table;
    table.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gip(*pInstance, "vkGetInstanceProcAddr");
    table.DestroyInstance = (PFN_vkDestroyInstance)gip(*pInstance, "vkDestroyInstance");
    table.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gip(*pInstance, "vkGetPhysicalDeviceMemoryProperties");

    {
        std::lock_guard<std::mutex> l(global_lock);
        // 🛠️ 수정: 오브젝트 매핑용은 SAFE_KEY, 디스패치 테이블용은 GetKey를 명확히 구분합니다.
        instanceMap[SAFE_KEY(*pInstance)] = *pInstance;
        instanceDispatch[GetKey(*pInstance)] = table;
    }
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
    if (!instance) return;
    std::lock_guard<std::mutex> l(global_lock);
    
    // 🔑 디스패치 테이블 해제는 GetKey
    auto it = instanceDispatch.find(GetKey(instance));
    if (it != instanceDispatch.end()) {
        it->second.DestroyInstance(instance, pAllocator);
        instanceDispatch.erase(it);
    }
    // 🛠️ 수정: 오브젝트 추적용 맵 삭제는 SAFE_KEY 사용
    instanceMap.erase(SAFE_KEY(instance));
    
    // 연관된 physicalDevice 매핑도 정리
    for (auto itPhys = physDevToInstance.begin(); itPhys != physDevToInstance.end();) {
        if (itPhys->second == instance) {
            itPhys = physDevToInstance.erase(itPhys);
        } else {
            ++itPhys;
        }
    }
}

// 🛠️ 수정: 물리 장치 매핑 저장 헬퍼 함수는 고유 주소 추적을 위해 SAFE_KEY를 사용해야 합니다.
static void BindPhysicalDeviceToInstance(VkPhysicalDevice physicalDevice, VkInstance instance) {
    if (physicalDevice == VK_NULL_HANDLE || instance == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> l(global_lock);
    physDevToInstance[SAFE_KEY(physicalDevice)] = instance;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
    VkResult result;
    VkLayerDeviceCreateInfo *layerCreateInfo = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;
    VkDeviceCreateInfo createInfo = *pCreateInfo;

    while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
        layerCreateInfo = (VkLayerDeviceCreateInfo *)layerCreateInfo->pNext;
    }
    if (layerCreateInfo == NULL) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    VkInstance instance = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> l(global_lock);
        // 🛠️ 수정: 물리 장치-인스턴스 고유 관계 추적은 고유 핸들 매핑 테이블이므로 SAFE_KEY를 사용합니다.
        auto it = physDevToInstance.find(SAFE_KEY(physicalDevice));
        if (it != physDevToInstance.end()) {
            instance = it->second;
        } else if (!instanceMap.empty()) {
            instance = instanceMap.begin()->second;
            physDevToInstance[SAFE_KEY(physicalDevice)] = instance;
        }
    }
    
    std::vector<const char *> enabledExtensions;
    if (!createInfo.ppEnabledExtensionNames) {
        enabledExtensions.reserve(4);
    } else {
        enabledExtensions.reserve(createInfo.enabledExtensionCount + 4);
        enabledExtensions.insert(enabledExtensions.end(), createInfo.ppEnabledExtensionNames, createInfo.ppEnabledExtensionNames + createInfo.enabledExtensionCount);
    }
    
    enabledExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    enabledExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    enabledExtensions.push_back(VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME);
    enabledExtensions.push_back(VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);
    
    createInfo.enabledExtensionCount = enabledExtensions.size();
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();

    PFN_vkCreateDevice createDevice = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    result = createDevice(physicalDevice, &createInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) return result;

    VkLayerDispatchTable table;
    table.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gdpa(*pDevice, "vkGetDeviceProcAddr");
    table.CreateImage = (PFN_vkCreateImage)gdpa(*pDevice, "vkCreateImage");
    table.CreateImageView = (PFN_vkCreateImageView)gdpa(*pDevice, "vkCreateImageView");
    table.AllocateMemory = (PFN_vkAllocateMemory)gdpa(*pDevice, "vkAllocateMemory");
    table.BindImageMemory = (PFN_vkBindImageMemory)gdpa(*pDevice, "vkBindImageMemory");
    table.GetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)gdpa(*pDevice, "vkGetImageMemoryRequirements");
    table.QueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(*pDevice, "vkQueueSubmit2");
    table.DeviceWaitIdle = (PFN_vkDeviceWaitIdle)gdpa(*pDevice, "vkDeviceWaitIdle");
    table.DestroyImage = (PFN_vkDestroyImage)gdpa(*pDevice, "vkDestroyImage");
    table.FreeMemory = (PFN_vkFreeMemory)gdpa(*pDevice, "vkFreeMemory");
    table.CreateFence = (PFN_vkCreateFence)gdpa(*pDevice, "vkCreateFence");
    table.AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)gdpa(*pDevice, "vkAllocateCommandBuffers");
    table.BeginCommandBuffer = (PFN_vkBeginCommandBuffer)gdpa(*pDevice, "vkBeginCommandBuffer");
    table.EndCommandBuffer = (PFN_vkEndCommandBuffer)gdpa(*pDevice, "vkEndCommandBuffer");
    table.ResetCommandBuffer = (PFN_vkResetCommandBuffer)gdpa(*pDevice, "vkResetCommandBuffer");
    table.CreateCommandPool = (PFN_vkCreateCommandPool)gdpa(*pDevice, "vkCreateCommandPool");
    table.DestroyCommandPool = (PFN_vkDestroyCommandPool)gdpa(*pDevice, "vkDestroyCommandPool");
    table.FreeCommandBuffers = (PFN_vkFreeCommandBuffers)gdpa(*pDevice, "vkFreeCommandBuffers");
    table.CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)gdpa(*pDevice, "vkCmdPipelineBarrier");
    table.CmdBlitImage = (PFN_vkCmdBlitImage)gdpa(*pDevice, "vkCmdBlitImage");
    table.CreateSemaphore = (PFN_vkCreateSemaphore)gdpa(*pDevice, "vkCreateSemaphore");
    table.DestroySemaphore = (PFN_vkDestroySemaphore)gdpa(*pDevice, "vkDestroySemaphore");
    table.WaitForFences = (PFN_vkWaitForFences)gdpa(*pDevice, "vkWaitForFences");
    table.ResetFences = (PFN_vkResetFences)gdpa(*pDevice, "vkResetFences");
    table.DestroyFence = (PFN_vkDestroyFence)gdpa(*pDevice, "vkDestroyFence");
    table.GetFenceFdKHR = (PFN_vkGetFenceFdKHR)gdpa(*pDevice, "vkGetFenceFdKHR");
    table.QueueSubmit = (PFN_vkQueueSubmit)gdpa(*pDevice, "vkQueueSubmit");
    table.GetDeviceQueue = (PFN_vkGetDeviceQueue)gdpa(*pDevice, "vkGetDeviceQueue");
    table.GetDeviceQueue2 = (PFN_vkGetDeviceQueue2)gdpa(*pDevice, "vkGetDeviceQueue2");
    table.DestroyDevice = (PFN_vkDestroyDevice)gdpa(*pDevice, "vkDestroyDevice");

    auto dev_node = std::make_shared<struct device>();
    dev_node->handle = *pDevice;
    dev_node->physical = physicalDevice;
    dev_node->table = table;

    dev_node->GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)gdpa(*pDevice, "vkGetMemoryFdKHR");
    dev_node->MapMemory      = (PFN_vkMapMemory)gdpa(*pDevice, "vkMapMemory");
    dev_node->UnmapMemory    = (PFN_vkUnmapMemory)gdpa(*pDevice, "vkUnmapMemory");

    {
        std::lock_guard<std::mutex> l(global_lock);
        // 🔑 디바이스 디스패치 저장소 등록은 GetKey를 사용합니다.
        deviceDispatch[GetKey(*pDevice)] = dev_node;
    }
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    if (!device) return;
    std::lock_guard<std::mutex> l(global_lock);
    // 🔑 디바이스 디스패치 검색은 GetKey를 사용합니다.
    auto it = deviceDispatch.find(GetKey(device));
    if (it != deviceDispatch.end()) {
        it->second->table.DestroyDevice(device, pAllocator);
        deviceDispatch.erase(it);
    }
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue)
{
    std::lock_guard<std::mutex> l(global_lock);
    // 🔑 디바이스 디스패치 테이블 접근은 GetKey
    auto dev = deviceDispatch[GetKey(device)];
    if (!dev) return;

    dev->table.GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    if (!pQueue || *pQueue == VK_NULL_HANDLE) return;

    auto q_node = std::make_shared<struct queue>();
    q_node->device = dev;
    q_node->handle = *pQueue;

    q_node->fence = std::make_unique<struct fence>();
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    dev->table.CreateFence(device, &fenceInfo, nullptr, &q_node->fence->handle);

    // 🛠️ 수정: 동일한 디바이스 내 다중 큐들의 키 충돌을 방지하기 위해 SAFE_KEY(*pQueue)를 사용합니다.
    queues[SAFE_KEY(*pQueue)] = q_node;
    dev->queue = q_node; 
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue *pQueue)
{
    std::lock_guard<std::mutex> l(global_lock);
    // 🔑 디바이스 디스패치 테이블 접근은 GetKey
    auto dev = deviceDispatch[GetKey(device)];
    if (!dev) return;

    dev->table.GetDeviceQueue2(device, pQueueInfo, pQueue);
    if (!pQueue || *pQueue == VK_NULL_HANDLE) return;

    auto q_node = std::make_shared<struct queue>();
    q_node->device = dev;
    q_node->handle = *pQueue;

    q_node->fence = std::make_unique<struct fence>();
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    dev->table.CreateFence(device, &fenceInfo, nullptr, &q_node->fence->handle);

    // 🛠️ 수정: 동일한 디바이스 내 다중 큐들의 키 충돌을 방지하기 위해 SAFE_KEY(*pQueue)를 사용합니다.
    queues[SAFE_KEY(*pQueue)] = q_node;
    dev->queue = q_node;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL 
DisplayX_CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
    struct fake_surface *fake_surf = (struct fake_surface *)::calloc(1, sizeof(struct fake_surface));
    if (!fake_surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    fake_surf->loader_magic = ICD_LOADER_MAGIC;
    fake_surf->obj_type = VK_OBJECT_TYPE_SURFACE_KHR;
    fake_surf->conn = pCreateInfo->connection; 
    fake_surf->window = pCreateInfo->window;   
    fake_surf->instance = instance;

    *pSurface = (VkSurfaceKHR)(uintptr_t)fake_surf; 
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR *pSurface)
{
    struct fake_surface *fake_surf = (struct fake_surface *)::calloc(1, sizeof(struct fake_surface));
    if (!fake_surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    fake_surf->loader_magic = ICD_LOADER_MAGIC;
    fake_surf->obj_type = VK_OBJECT_TYPE_SURFACE_KHR;
    fake_surf->conn = XGetXCBConnection(pCreateInfo->dpy); 
    fake_surf->window = pCreateInfo->window;
    fake_surf->instance = instance;
    
    *pSurface = (VkSurfaceKHR)(uintptr_t)fake_surf;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported)
{
    *pSupported = VK_TRUE;
    return VK_SUCCESS;    
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator)
{
    struct fake_surface *fake_surf = (struct fake_surface *)(uintptr_t)surface;
    if (fake_surf) ::free(fake_surf);
}

// --- [Swapchain Runtime Processing Engine] ---

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    std::shared_ptr<struct device> dev;
    {
        std::lock_guard<std::mutex> l(global_lock);
        // 🔑 디바이스 디스패치 테이블 획득은 GetKey
        dev = deviceDispatch[GetKey(device)];
    }
    VkLayerDispatchTable table = dev->table;

    struct fake_swapchain *swapchain = (struct fake_swapchain *)::calloc(1, sizeof(struct fake_swapchain));
    if (!swapchain) return VK_ERROR_OUT_OF_HOST_MEMORY;
    
    swapchain->loader_magic = ICD_LOADER_MAGIC;
    swapchain->obj_type = VK_OBJECT_TYPE_SWAPCHAIN_KHR;
    swapchain->wsi_device = device;
    swapchain->imageCount = pCreateInfo->minImageCount < 3 ? 3 : pCreateInfo->minImageCount; 
    swapchain->format = pCreateInfo->imageFormat;
    swapchain->extent = pCreateInfo->imageExtent;
    swapchain->device = dev;

    struct fake_surface *fake_surface = (struct fake_surface *)(uintptr_t)pCreateInfo->surface;
    if (fake_surface == nullptr) { ::free(swapchain); return VK_ERROR_SURFACE_LOST_KHR; }
    swapchain->surface = fake_surface;
    swapchain->images.resize(swapchain->imageCount);
    
    for (uint32_t index = 0; index < swapchain->imageCount; index++) {
        VkResult result;
        auto fake_image = std::make_shared<struct fake_swapchain_image>();
        fake_image->width = swapchain->extent.width;
        fake_image->height = swapchain->extent.height;

        VkExternalMemoryImageCreateInfo externalCreateInfo{};
        externalCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT; 

        VkImageCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        createInfo.pNext = &externalCreateInfo;
        createInfo.imageType = VK_IMAGE_TYPE_2D;
        createInfo.format = swapchain->format;
        createInfo.extent = {fake_image->width, fake_image->height, 1};
        createInfo.mipLevels = 1;
        createInfo.arrayLayers = 1;
        createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        createInfo.usage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; 
        createInfo.sharingMode = pCreateInfo->imageSharingMode;    
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        result = table.CreateImage(device, &createInfo, nullptr, &fake_image->handle);
        if (result != VK_SUCCESS) { ::free(swapchain); return result; }

        VkMemoryRequirements memReqs;
        table.GetImageMemoryRequirements(device, fake_image->handle, &memReqs);

        VkExportMemoryAllocateInfo exportAllocInfo{};
        exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.pNext = &exportAllocInfo;
        allocateInfo.allocationSize = memReqs.size;
        allocateInfo.memoryTypeIndex = pick_memory_index(swapchain->surface->instance, dev->physical, memReqs.memoryTypeBits);

        result = table.AllocateMemory(device, &allocateInfo, nullptr, &fake_image->memory);
        if (result != VK_SUCCESS) { ::free(swapchain); return result; }
            
        result = table.BindImageMemory(device, fake_image->handle, fake_image->memory, 0);
        if (result != VK_SUCCESS) { ::free(swapchain); return result; }

        swapchain->images[index] = fake_image;
    }

    *pSwapchain = (VkSwapchainKHR)(uintptr_t)swapchain;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages)
{
    struct fake_swapchain *fake_swapchain = (struct fake_swapchain *)(uintptr_t)swapchain;
    if (fake_swapchain == nullptr) return VK_ERROR_OUT_OF_DATE_KHR;

    uint32_t reportCount = fake_swapchain->imageCount; 

    if (pSwapchainImages == nullptr) {
        *pSwapchainImageCount = reportCount;
        return VK_SUCCESS;
    }

    uint32_t count = std::min(*pSwapchainImageCount, reportCount);
    for (uint32_t i = 0; i < count; i++) {
        pSwapchainImages[i] = fake_swapchain->images[i]->handle;
    }
    *pSwapchainImageCount = count;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    struct fake_swapchain *fake_swapchain = (struct fake_swapchain *)(uintptr_t)swapchain;
    if (fake_swapchain == nullptr || (uintptr_t)fake_swapchain < 0x1000) return VK_ERROR_OUT_OF_DATE_KHR;

    if (fence != VK_NULL_HANDLE || semaphore != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> l(global_lock);
        // 🔑 디바이스 디스패치 테이블 검색은 GetKey
        auto dev = deviceDispatch[GetKey(device)];                                                     
        auto q = dev->queue;
        if (q && q->handle != VK_NULL_HANDLE) {
            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.signalSemaphoreCount = (semaphore != VK_NULL_HANDLE) ? 1 : 0;
            submitInfo.pSignalSemaphores = (semaphore != VK_NULL_HANDLE) ? &semaphore : nullptr;
            dev->table.QueueSubmit(q->handle, 1, &submitInfo, fence);
        }
    }

    {
        std::lock_guard<std::mutex> l(global_lock);
        *pImageIndex = fake_swapchain->currentImage;
        fake_swapchain->currentImage = (fake_swapchain->currentImage + 1) % fake_swapchain->imageCount;
    }
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t *pImageIndex)
{
    struct fake_swapchain *fake_swapchain = (struct fake_swapchain *)(uintptr_t)pAcquireInfo->swapchain;
    if (fake_swapchain == nullptr || (uintptr_t)fake_swapchain < 0x1000) return VK_ERROR_OUT_OF_DATE_KHR;

    if (pAcquireInfo->fence != VK_NULL_HANDLE || pAcquireInfo->semaphore != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> l(global_lock);
        // 🔑 디바이스 디스패치 테이블 검색은 GetKey
        auto dev = deviceDispatch[GetKey(device)];
        auto q = dev->queue;
        if (q && q->handle != VK_NULL_HANDLE) {
            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.signalSemaphoreCount = (pAcquireInfo->semaphore != VK_NULL_HANDLE) ? 1 : 0;
            submitInfo.pSignalSemaphores = (pAcquireInfo->semaphore != VK_NULL_HANDLE) ? &pAcquireInfo->semaphore : nullptr;
            dev->table.QueueSubmit(q->handle, 1, &submitInfo, pAcquireInfo->fence);
        }
    }

    {
        std::lock_guard<std::mutex> l(global_lock);
        *pImageIndex = fake_swapchain->currentImage;
        fake_swapchain->currentImage = (fake_swapchain->currentImage + 1) % fake_swapchain->imageCount;
    }
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
    struct fake_swapchain *fake_swapchain = (struct fake_swapchain *)(uintptr_t)swapchain;
    std::shared_ptr<struct device> dev;
    {
        std::lock_guard<std::mutex> l(global_lock);
        // 🔑 디바이스 디스패치 테이블 검색은 GetKey
        dev = deviceDispatch[GetKey(device)];
    }
    if (!fake_swapchain || !dev) return;

    dev->table.DeviceWaitIdle(device);
    std::lock_guard<std::mutex> l(global_lock);
	
    for (uint32_t index = 0; index < fake_swapchain->images.size(); index++) {
        auto swapchain_image = fake_swapchain->images[index];
        dev->table.FreeMemory(device, swapchain_image->memory, nullptr);
        dev->table.DestroyImage(device, swapchain_image->handle, nullptr);
    }
    fake_swapchain->images.clear();
    ::free(fake_swapchain);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    std::shared_ptr<struct queue> q = nullptr;
    {
        std::lock_guard<std::mutex> l(global_lock);
        // 🛠️ 수정: 큐 추적 맵 검색은 고유 핸들 기반이므로 SAFE_KEY를 사용해야 올바르게 바인딩됩니다.
        auto it = queues.find(SAFE_KEY(queue));
        if (it != queues.end()) {
            q = it->second;
        }
    }

    if (!q || !q->device || !q->fence || !q->fence->handle) {
        std::lock_guard<std::mutex> l(global_lock);
        for (auto& pair : deviceDispatch) {
            if (pair.second->table.QueuePresentKHR) {
                return pair.second->table.QueuePresentKHR(queue, pPresentInfo);
            }
        }
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    q->device->table.ResetFences(q->device->handle, 1, &q->fence->handle);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    submitInfo.pWaitDstStageMask = waitStages.data();
    submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
    submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;

    q->device->table.QueueSubmit(q->handle, 1, &submitInfo, q->fence->handle);
    q->device->table.WaitForFences(q->device->handle, 1, &q->fence->handle, VK_TRUE, UINT64_MAX); 

    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        struct fake_swapchain *fake_swapchain = (struct fake_swapchain *)(uintptr_t)pPresentInfo->pSwapchains[i];
        if (!fake_swapchain || !fake_swapchain->surface) continue;
        
        uint32_t imgIdx = pPresentInfo->pImageIndices[i];
        auto target_image = fake_swapchain->images[imgIdx];

        uint32_t window = fake_swapchain->surface->window;
        if (window == 0) continue;

        int dma_buf_fd = -1;
        
        if (q->device && q->device->GetMemoryFdKHR) {
            VkMemoryGetFdInfoKHR getFdInfo{};
            getFdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            getFdInfo.memory = target_image->memory;
            getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            
            q->device->GetMemoryFdKHR(q->device->handle, &getFdInfo, &dma_buf_fd);
        }

        if (dma_buf_fd >= 0) { 
            send_dma_buf_to_x11_app(
                window, 
                imgIdx, 
                dma_buf_fd, 
                fake_swapchain->extent.width, 
                fake_swapchain->extent.height,
                (uint32_t)fake_swapchain->format
            );
            ::close(dma_buf_fd); 
        }
    }

    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities) 
{
    if (!pSurfaceCapabilities) return VK_SUCCESS;

    pSurfaceCapabilities->sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    
    VkSurfaceCapabilitiesKHR* caps = &pSurfaceCapabilities->surfaceCapabilities;
    caps->minImageCount = 2;
    caps->maxImageCount = 8;
    caps->currentExtent = {0xFFFFFFFF, 0xFFFFFFFF};
    caps->minImageExtent = {1, 1};
    caps->maxImageExtent = {8112, 8112};
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    void* pNext = pSurfaceCapabilities->pNext;
    while (pNext) {
        VkBaseOutStructure* outStruct = reinterpret_cast<VkBaseOutStructure*>(pNext);
        if (outStruct->sType == VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR) {
            VkSurfaceProtectedCapabilitiesKHR* protectedCaps = reinterpret_cast<VkSurfaceProtectedCapabilitiesKHR*>(pNext);
            protectedCaps->supportsProtected = VK_FALSE;
        }
        pNext = outStruct->pNext;
    }

    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL DisplayX_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormat2KHR* pSurfaceFormats)
{
    if (pSurfaceFormats == nullptr) {
        *pSurfaceFormatCount = 1;
        return VK_SUCCESS;
    }
    
    pSurfaceFormats[0].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
    pSurfaceFormats[0].pNext = nullptr;
    pSurfaceFormats[0].surfaceFormat.format = VK_FORMAT_B8G8R8A8_UNORM; 
    pSurfaceFormats[0].surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    *pSurfaceFormatCount = 1;
    
    return VK_SUCCESS;
}

extern "C" {

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
DisplayX_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
    GETPROCADDR(CreateInstance);
    GETPROCADDR(DestroyInstance);
    GETPROCADDR(CreateDevice);
    GETPROCADDR(DestroyDevice);
    GETPROCADDR(CreateXcbSurfaceKHR);
    GETPROCADDR(CreateXlibSurfaceKHR);
    GETPROCADDR(GetPhysicalDeviceSurfaceSupportKHR);
    GETPROCADDR(GetDeviceQueue);
    GETPROCADDR(GetDeviceQueue2);
    GETPROCADDR(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    GETPROCADDR(GetPhysicalDeviceSurfaceFormatsKHR);
    GETPROCADDR(GetPhysicalDeviceSurfacePresentModesKHR);
    GETPROCADDR(DestroySurfaceKHR);
    GETPROCADDR(GetPhysicalDeviceSurfaceCapabilities2KHR);
    GETPROCADDR(GetPhysicalDeviceSurfaceFormats2KHR);
    
    if (!strcmp(pName, "vkGetInstanceProcAddr")) 
        return (PFN_vkVoidFunction)&DisplayX_GetInstanceProcAddr;

    if (instance == VK_NULL_HANDLE) return nullptr;
    
    std::lock_guard<std::mutex> l(global_lock);
    // 🔑 디스패치 테이블 조회이므로 GetKey 사용 (올바름)
    auto it = instanceDispatch.find(GetKey(instance));
    if (it == instanceDispatch.end()) return nullptr;
    
    return it->second.GetInstanceProcAddr(instance, pName);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
DisplayX_GetDeviceProcAddr(VkDevice device, const char *pName)
{
    GETPROCADDR(GetSwapchainImagesKHR);
    GETPROCADDR(AcquireNextImageKHR);
    GETPROCADDR(AcquireNextImage2KHR);
    GETPROCADDR(CreateSwapchainKHR);
    GETPROCADDR(DestroySwapchainKHR);
    GETPROCADDR(QueuePresentKHR);
    GETPROCADDR(WaitForPresentKHR);
    GETPROCADDR(GetDeviceQueue);
    GETPROCADDR(GetDeviceQueue2);
    GETPROCADDR(DestroyDevice);

    if (!strcmp(pName, "vkGetDeviceProcAddr")) 
        return (PFN_vkVoidFunction)&DisplayX_GetDeviceProcAddr;

    if (device == VK_NULL_HANDLE) return nullptr;

    std::lock_guard<std::mutex> l(global_lock);
    // 🔑 디바이스 디스패치 테이블 조회이므로 GetKey 사용 (올바름)
    auto it = deviceDispatch.find(GetKey(device));
    if (it == deviceDispatch.end()) return nullptr;

    return it->second->table.GetDeviceProcAddr(device, pName);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return DisplayX_GetInstanceProcAddr(instance, pName);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    return DisplayX_GetDeviceProcAddr(device, pName);
}

}
