#include "displayx_layer.hpp"
#include <vector>
#include <unistd.h>
// #include <sys/socket.h> -> 폐기 (IPC 필요 없음)
// #include <sys/un.h>     -> 폐기

#define GETPROCADDR(func) \
if (!strcmp(pName, "vk" #func)) \
    return (PFN_vkVoidFunction)&DisplayX_##func;

std::unordered_map<void *, VkLayerInstanceDispatchTable> instanceDispatch;
std::unordered_map<void *, VkInstance> instanceMap;
std::unordered_map<void *, std::shared_ptr<struct device>> deviceDispatch;                             
std::unordered_map<VkQueue, std::shared_ptr<struct queue>> queues;
ID id;
std::mutex global_lock;

// --- [Utility Functions] ---
int pick_memory_index(VkInstance instance, VkPhysicalDevice physical, uint32_t memoryBits) {
    VkPhysicalDeviceMemoryProperties memoryProps{};
    uint32_t idx;
    instanceDispatch[GetKey(instance)].GetPhysicalDeviceMemoryProperties(physical, &memoryProps);
    for (idx = 0; idx < memoryProps.memoryTypeCount; idx++) {
        if (memoryBits & (1u << idx))
            return idx;
    }
    return UINT32_MAX;
}

// --- [Vulkan Core Intercepts] ---

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, uint64_t flags)
{
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
    // 하드웨어 버퍼(FD)를 주고받기 위해 리눅스 표준 인스턴스 확장 셋업
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
		scoped_lock l(global_lock);
    	instanceMap[GetKey(*pInstance)] = *pInstance;
  		instanceDispatch[GetKey(*pInstance)] = table;
	}
    return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
	if (!instance) return;
	scoped_lock l(global_lock);
	VkLayerInstanceDispatchTable table = instanceDispatch[GetKey(instance)];
	table.DestroyInstance(instance, pAllocator);
	instanceDispatch.erase(GetKey(instance));
	instanceMap.erase(GetKey(instance));
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

    VkInstance instance = instanceMap[GetKey(physicalDevice)];
    
    std::vector<const char *> enabledExtensions;
    if (!createInfo.ppEnabledExtensionNames) {
    	enabledExtensions.reserve(4);
    } else {
    	enabledExtensions.reserve(createInfo.enabledExtensionCount + 4);
    	enabledExtensions.insert(enabledExtensions.end(), createInfo.ppEnabledExtensionNames, createInfo.ppEnabledExtensionNames + createInfo.enabledExtensionCount);
    }
    
    // ⭐ 핵심 변경: 안드로이드 AHB를 걷어내고 리눅스 표준 dma-buf 패싱용 확장 주입
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
    
    // 하부 드라이버 주소 할당부 테이블 등록
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

    // ⭐ 수정: 변수명을 dev_node로 선언하여 매개변수 device와의 충돌 완전 방지
    auto dev_node = std::make_shared<struct device>();
    dev_node->handle = *pDevice;
    dev_node->physical = physicalDevice;
    dev_node->table = table;

    // ⭐ 수정: 헤더파일 구조체에 선언한 포인터 필드에 안전하게 정렬 바인딩
    dev_node->GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)gdpa(*pDevice, "vkGetMemoryFdKHR");
    dev_node->MapMemory      = (PFN_vkMapMemory)gdpa(*pDevice, "vkMapMemory");
    dev_node->UnmapMemory    = (PFN_vkUnmapMemory)gdpa(*pDevice, "vkUnmapMemory");

	{
		scoped_lock l(global_lock);
    	deviceDispatch[GetKey(*pDevice)] = dev_node; // 맵에 노드 주입
	}
	return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL 
DisplayX_CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
    // 외부 프로세스 대기 없이 즉시 통과
    struct fake_surface *fake_surf = (struct fake_surface *)calloc(1, sizeof(struct fake_surface));
    if (!fake_surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    fake_surf->loader_magic = ICD_LOADER_MAGIC;
    fake_surf->obj_type = VK_OBJECT_TYPE_SURFACE_KHR;
    fake_surf->conn = pCreateInfo->connection; // ⭐ 낚아채기 1: X11 연결 포인터
    fake_surf->window = pCreateInfo->window;   // ⭐ 낚아채기 2: X11 윈도우 ID
    fake_surf->instance = instance;

    *pSurface = VK_WRAP_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR, fake_surf);
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR *pSurface)
{
    struct fake_surface *fake_surf = (struct fake_surface *)calloc(1, sizeof(struct fake_surface));
    if (!fake_surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    fake_surf->loader_magic = ICD_LOADER_MAGIC;
    fake_surf->obj_type = VK_OBJECT_TYPE_SURFACE_KHR;
    fake_surf->conn = XGetXCBConnection(pCreateInfo->dpy); // Xlib 구조도 XCB 커넥션으로 환원
    fake_surf->window = pCreateInfo->window;
    fake_surf->instance = instance;
    
    *pSurface = VK_WRAP_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR, fake_surf);
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported)
{
    *pSupported = VK_TRUE;
    return VK_SUCCESS;    
}

// --- [WSI Capabilities Enforcements] ---
// GetPhysicalDeviceSurfaceCapabilitiesKHR / GetPhysicalDeviceSurfaceFormatsKHR 등의 메타데이터 쿼리 함수들은 
// 기존의 형식을 그대로 유지하되 안전하게 작동합니다. (코드가 길어 생략하나 기존 형태 유지)


// --- [Swapchain Runtime Processing Engine - 리눅스 dma-buf 기반 재설계] ---

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    auto dev = deviceDispatch[GetKey(device)];                              
    VkLayerDispatchTable table = dev->table;

    struct fake_swapchain *swapchain = (struct fake_swapchain *)calloc(1, sizeof(struct fake_swapchain));
    if (!swapchain) return VK_ERROR_OUT_OF_HOST_MEMORY;
    
    swapchain->loader_magic = ICD_LOADER_MAGIC;
    swapchain->obj_type = VK_OBJECT_TYPE_SWAPCHAIN_KHR;
    swapchain->wsi_device = device;
    swapchain->imageCount = pCreateInfo->minImageCount < 3 ? 3 : pCreateInfo->minImageCount; // 안정적인 3버퍼 체제 최소 보장
    swapchain->format = pCreateInfo->imageFormat;
    swapchain->extent = pCreateInfo->imageExtent;
    swapchain->device = dev;

    VK_UNWRAP_NON_DISPATCHABLE_HANDLE(pCreateInfo->surface, struct fake_surface , fake_surface);
    if (fake_surface == nullptr) { free(swapchain); return VK_ERROR_SURFACE_LOST_KHR; }
    swapchain->surface = fake_surface;
    swapchain->images.resize(swapchain->imageCount);
    
    // 리눅스 dma-buf (External Memory Handle FD) 추출 구조로 이미지 생성
    for (uint32_t index = 0; index < swapchain->imageCount; index++) {
        VkResult result;
        auto fake_image = std::make_shared<struct fake_swapchain_image>();
        fake_image->width = swapchain->extent.width;
        fake_image->height = swapchain->extent.height;

        // 리눅스 표준 외부 메모리 내보내기 구조체 설정
        VkExternalMemoryImageCreateInfo externalCreateInfo{};
        externalCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT; // dma-buf 호환 구조

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
        createInfo.usage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // 읽기 복사 대비 여유 플래그 추가
        createInfo.sharingMode = pCreateInfo->imageSharingMode;    
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        result = table.CreateImage(device, &createInfo, nullptr, &fake_image->handle);
        if (result != VK_SUCCESS) { free(swapchain); return result; }

        // 메모리 요구사항 쿼리 및 전용 메모리 할당
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
        if (result != VK_SUCCESS) { free(swapchain); return result; }
            
        result = table.BindImageMemory(device, fake_image->handle, fake_image->memory, 0);
        if (result != VK_SUCCESS) { free(swapchain); return result; }

        swapchain->images[index] = fake_image;
    }

    // 소켓 전송 패킷 및 ACK 루프 제거 (인프로세스 다이렉트 전환으로 불필요)

    *pSwapchain = VK_WRAP_NON_DISPATCHABLE_HANDLE(VkSwapchainKHR, swapchain);
    return VK_SUCCESS;
}

// --- [X11 Direct Injection Engine (Re-architected)] ---

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages)
{
	VK_UNWRAP_NON_DISPATCHABLE_HANDLE(swapchain, struct fake_swapchain, fake_swapchain)
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
	VK_UNWRAP_NON_DISPATCHABLE_HANDLE(swapchain, struct fake_swapchain, fake_swapchain)
	if (fake_swapchain == nullptr || (uintptr_t)fake_swapchain < 0x1000) return VK_ERROR_OUT_OF_DATE_KHR;

	if (fence != VK_NULL_HANDLE || semaphore != VK_NULL_HANDLE) {
		scoped_lock l(global_lock);
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
		scoped_lock l(global_lock);
		*pImageIndex = fake_swapchain->currentImage;
		fake_swapchain->currentImage = (fake_swapchain->currentImage + 1) % fake_swapchain->imageCount;
	}
	return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t *pImageIndex)
{
	VK_UNWRAP_NON_DISPATCHABLE_HANDLE(pAcquireInfo->swapchain, struct fake_swapchain, fake_swapchain)
	if (fake_swapchain == nullptr || (uintptr_t)fake_swapchain < 0x1000) return VK_ERROR_OUT_OF_DATE_KHR;

	if (pAcquireInfo->fence != VK_NULL_HANDLE || pAcquireInfo->semaphore != VK_NULL_HANDLE) {
		scoped_lock l(global_lock);
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
		scoped_lock l(global_lock);
		*pImageIndex = fake_swapchain->currentImage;
		fake_swapchain->currentImage = (fake_swapchain->currentImage + 1) % fake_swapchain->imageCount;
	}
	return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
	VK_UNWRAP_NON_DISPATCHABLE_HANDLE(swapchain, struct fake_swapchain, fake_swapchain)
	auto dev = deviceDispatch[GetKey(device)];
	if (!fake_swapchain || !dev) return;

	dev->table.DeviceWaitIdle(device);
	scoped_lock l(global_lock);
	
	for (uint32_t index = 0; index < fake_swapchain->images.size(); index++) {
		auto swapchain_image = fake_swapchain->images[index];
		dev->table.FreeMemory(device, swapchain_image->memory, nullptr);
        // 안드로이드 AHB 해제부 제거
		dev->table.DestroyImage(device, swapchain_image->handle, nullptr);
	}
	fake_swapchain->images.clear();
    // 소켓 소멸 패킷 전송부 완전히 삭제
	free(fake_swapchain);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    auto q = queues[queue];
    if (!q || !q->device || !q->fence) return VK_ERROR_OUT_OF_HOST_MEMORY;

    // 1. 기존 드라이버 동기화 가속화 처리
    q->device->table.ResetFences(q->device->handle, 1, &q->fence->handle);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    submitInfo.pWaitDstStageMask = waitStages.data();
    submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
    submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;

    // GPU 연산 명령 제출
    q->device->table.QueueSubmit(q->handle, 1, &submitInfo, q->fence->handle);

    // ⭐ 인프로세스 직접 찔러넣기를 위해 GPU 작업이 끝날 때까지 CPU가 완벽히 보장 대기 (Host Wait)
    q->device->table.WaitForFences(q->device->handle, 1, &q->fence->handle, VK_TRUE, UINT64_MAX); 

    // 기존의 무겁고 느린 fence_fd 추출 및 dup, 소켓 루프 전면 삭제!!

    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        VK_UNWRAP_NON_DISPATCHABLE_HANDLE(pPresentInfo->pSwapchains[i], struct fake_swapchain, fake_swapchain)
        if (!fake_swapchain || !fake_swapchain->surface) continue;
        
        uint32_t imgIdx = pPresentInfo->pImageIndices[i];
        auto target_image = fake_swapchain->images[imgIdx];

        // 2부에서 낚아챈 소중한 X11 자원 연결
        xcb_connection_t* conn = (xcb_connection_t*)fake_swapchain->surface->conn;
        xcb_window_t window = fake_swapchain->surface->window;
        if (!conn || window == 0) continue;

       // ⭐ 안전 보정: 디스패치 테이블에 보관한 포인터로 드라이버를 직접 호출하여 픽셀 주소 확보
        void* pixel_data = nullptr;
        VkResult res = q->device->MapMemory 
            ? q->device->MapMemory(q->device->handle, target_image->memory, 0, VK_WHOLE_SIZE, 0, &pixel_data)
            : VK_ERROR_INITIALIZATION_FAILED;

        if (res == VK_SUCCESS && pixel_data != nullptr) {
            // X11에서 그리기 도구 역할을 할 기본 임시 그래픽 컨텍스트(GC) 생성
            xcb_gcontext_t gc = xcb_generate_id(conn);
            xcb_create_gc(conn, gc, window, 0, nullptr);

            uint32_t width = fake_swapchain->extent.width;
            uint32_t height = fake_swapchain->extent.height;
            uint32_t image_size = width * height * 4; // RGBA 32bpp 표준 데이터 크기

            // 🚀 가로챈 X11 윈도우 창 핸들에 원본 픽셀 데이터 즉시 투하
            xcb_put_image(
                conn,
                XCB_IMAGE_FORMAT_Z_PIXMAP,
                window,
                gc,
                width, height,     // 캔버스 가로, 세로 해상도
                0, 0,              // 목적지 오프셋 좌표 (X, Y)
                0,                 // 레프트 패딩 값
                24,                // 컬러 뎁스 (화면 표시 깊이)
                image_size,        // 투하할 바이트 크기
                (const uint8_t*)pixel_data // GPU 드라이버가 그린 실시간 픽셀 포인터!
            );

            // X11 서버에 명령을 즉시 전송하여 화면에 동기화 구현
            xcb_flush(conn);
            xcb_free_gc(conn, gc);

            // ⭐ 수정: 다음 프레임을 위해 언맵 함수도 테이블 경유 호출로 안전하게 해제
            q->device->UnmapMemory(q->device->handle, target_image->memory);
        } else {
            Logger::log("error", "Direct injection failed: Unable to map memory of image index %d", imgIdx);
        }
    }

    return VK_SUCCESS;
}
