#include "displayx_layer.hpp"
#define GETPROCADDR(func) \
if (!strcmp(pName, "vk" #func)) \
    return (PFN_vkVoidFunction)&DisplayX_##func;

static int prefer_rgba8 = -1;

// --- [Utility Functions] ---
int to_ahardwarebuffer_format(VkFormat format) {
	switch (format) {
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_R8G8B8A8_UNORM:
			return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
			return AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM;
		default:
			return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
	}
}

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

void sendFD(int& socket, int fd) {
	std::vector<char> control_buffer(CMSG_SPACE(sizeof(int)));
	char dummy = 0;
	struct iovec iov{};
	iov.iov_len = 1;
	iov.iov_base = &dummy;
	struct msghdr msg{};
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control_buffer.data();
	msg.msg_controllen = control_buffer.size();

	struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	*reinterpret_cast<int*>(CMSG_DATA(cmsg)) = fd;
	sendmsg(socket, &msg, 0);
}

// --- [Vulkan Core Intercepts] ---

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, uint64_t flags)
{
    Logger::log("trace", "DisplayX_WaitForPresentKHR intercepted! Bypassing driver check.");
    return VK_SUCCESS; 
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{	
	Logger::log("trace", "Calling vkCreateInstance");
	VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;
	VkResult result;
	VkInstanceCreateInfo createInfo = *pCreateInfo;
	
    while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
    	layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
    }
    if (!layerCreateInfo) {
    	Logger::log("error", "Failed to query layerCreateInfo");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    PFN_vkGetInstanceProcAddr gip = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

	std::vector<const char *> enabledExtensions;
	if (!createInfo.ppEnabledExtensionNames) {
		enabledExtensions.reserve(1);
	} else {
		enabledExtensions.reserve(createInfo.enabledExtensionCount + 1);
		enabledExtensions.insert(enabledExtensions.end(), createInfo.ppEnabledExtensionNames, createInfo.ppEnabledExtensionNames + createInfo.enabledExtensionCount);
	}
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

    if (prefer_rgba8 == -1) prefer_rgba8 = getenv("PREFER_RGBA8") && atoi(getenv("PREFER_RGBA8"));

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
	Logger::log("trace", "Calling vkCreateDevice");
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
    	enabledExtensions.reserve(3);
    } else {
    	enabledExtensions.reserve(createInfo.enabledExtensionCount + 3);
    	enabledExtensions.insert(enabledExtensions.end(), createInfo.ppEnabledExtensionNames, createInfo.ppEnabledExtensionNames + createInfo.enabledExtensionCount);
    }
    enabledExtensions.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
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
    table.GetAndroidHardwareBufferPropertiesANDROID = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)gdpa(*pDevice, "vkGetAndroidHardwareBufferPropertiesANDROID");
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

    auto device = std::make_shared<struct device>();
    device->handle = *pDevice;
    device->physical = physicalDevice;
    device->table = table;

	{
		scoped_lock l(global_lock);
    	deviceDispatch[GetKey(*pDevice)] = device;
	}
	return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue)
{
	auto dev = deviceDispatch[GetKey(device)];
	dev->table.GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
	
	auto queue = std::make_shared<struct queue>();
	queue->device = dev;
	queue->handle = *pQueue;

	VkFence fence;
	VkExportFenceCreateInfo exportInfo{};
	exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
	exportInfo.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
	
	VkFenceCreateInfo fenceCreateInfo{};
	fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceCreateInfo.pNext = &exportInfo;
	
	dev->table.CreateFence(device, &fenceCreateInfo, nullptr, &fence);	
	auto f = std::make_unique<struct fence>();
	f->handle = fence;
	f->sync_fd = -1;
	queue->fence = std::move(f);
	      
	{
		scoped_lock l(global_lock);
		queues[*pQueue] = queue;
		dev->queue = queue;
	}
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue *pQueue)
{
	auto dev = deviceDispatch[GetKey(device)];
	dev->table.GetDeviceQueue2(device, pQueueInfo, pQueue);
	
	auto queue = std::make_shared<struct queue>();
    queue->device = dev;
    queue->handle = *pQueue;

    VkFence fence;
    VkExportFenceCreateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    
    VkFenceCreateInfo fenceCreateInfo{};
    fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreateInfo.pNext = &exportInfo;
    dev->table.CreateFence(device, &fenceCreateInfo, nullptr, &fence);
    
    auto f = std::make_unique<struct fence>();
    f->handle = fence;
    f->sync_fd = -1;
    queue->fence = std::move(f);
    
    {
    	scoped_lock l(global_lock);
     	queues[*pQueue] = queue;
     	dev->queue = queue;
    }
}

// --- [Surface Infrastructure (Calloc Allocation for Stability)] ---

VK_LAYER_EXPORT VkResult VKAPI_CALL 
DisplayX_CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
	Logger::log("trace","Calling vkCreateXcbSurfaceKHR");
	struct fake_surface *fake_surf = (struct fake_surface *)calloc(1, sizeof(struct fake_surface));
	if (!fake_surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

	fake_surf->loader_magic = ICD_LOADER_MAGIC;
	fake_surf->obj_type = VK_OBJECT_TYPE_SURFACE_KHR;
	fake_surf->conn = pCreateInfo->connection;
	fake_surf->window = pCreateInfo->window;
	fake_surf->native_renderer_fd = socket(AF_UNIX, SOCK_STREAM, 0);                  
	fake_surf->instance = instance;

	struct sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	const char *sock_name = "/data/data/com.termux/files/usr/tmp/.X11-unix/X0";
	size_t name_len = strlen(sock_name);
	addr.sun_path[0] = '\0';
	memcpy(addr.sun_path + 1, sock_name, name_len);
	
	socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + name_len;
	if (connect(fake_surf->native_renderer_fd, (struct sockaddr *)&addr, len) != 0) {
		close(fake_surf->native_renderer_fd);
		free(fake_surf); 
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	*pSurface = VK_WRAP_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR, fake_surf);
	return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR *pSurface)
{
	Logger::log("trace", "Calling vkCreateXlibSurfaceKHR");
	struct fake_surface *fake_surf = (struct fake_surface *)calloc(1, sizeof(struct fake_surface));
	if (!fake_surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

	fake_surf->loader_magic = ICD_LOADER_MAGIC;
	fake_surf->obj_type = VK_OBJECT_TYPE_SURFACE_KHR;
	fake_surf->conn = XGetXCBConnection(pCreateInfo->dpy);
	fake_surf->window = pCreateInfo->window;
	fake_surf->native_renderer_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	fake_surf->instance = instance;
	
	struct sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	const char *sock_name = "/data/data/com.termux/files/usr/tmp/.X11-unix/X0";
	size_t name_len = strlen(sock_name);
	addr.sun_path[0] = '\0';
	memcpy(addr.sun_path + 1, sock_name, name_len);
	
	socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + name_len;
	if (connect(fake_surf->native_renderer_fd, (struct sockaddr *)&addr, len) != 0) {                                                                                    
		close(fake_surf->native_renderer_fd);
		free(fake_surf);                                                
		return VK_ERROR_INITIALIZATION_FAILED;                                                     
	}
	*pSurface = VK_WRAP_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR, fake_surf);
	return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported)
{
	*pSupported = VK_TRUE;
	return VK_SUCCESS;	
}

// --- [WSI Capabilities Enforcements (Forced 3-Buffer Integration)] ---

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
{
	if (surface == VK_NULL_HANDLE || pSurfaceCapabilities == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
	VK_UNWRAP_NON_DISPATCHABLE_HANDLE(surface, struct fake_surface, fake_surface)
	if (!fake_surface || !fake_surface->conn) return VK_ERROR_SURFACE_LOST_KHR;

	pSurfaceCapabilities->minImageCount = 3;
	pSurfaceCapabilities->maxImageCount = 3;

	xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry(fake_surface->conn, fake_surface->window);
	xcb_get_geometry_reply_t *geom_rep = xcb_get_geometry_reply(fake_surface->conn, geom_cookie, nullptr);
	VkExtent2D extent;
	if (geom_rep != nullptr) {
		extent.width = geom_rep->width;
		extent.height = geom_rep->height;
		free(geom_rep);
	} else {
		extent.width = 1280;
		extent.height = 720;
	}
	
	pSurfaceCapabilities->currentExtent = extent;
	pSurfaceCapabilities->maxImageExtent = extent;
	pSurfaceCapabilities->minImageExtent = extent;
	pSurfaceCapabilities->maxImageArrayLayers = 1;
	pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	pSurfaceCapabilities->supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	return VK_SUCCESS;	
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceSurfaceInfo2KHR *pInfo, VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
	if (!pInfo || pInfo->surface == VK_NULL_HANDLE || !pSurfaceCapabilities) return VK_ERROR_INITIALIZATION_FAILED;

	VkResult result = DisplayX_GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, pInfo->surface, &pSurfaceCapabilities->surfaceCapabilities);

	return result;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL 
DisplayX_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats)
{
	if (pSurfaceFormats == nullptr) {
		*pSurfaceFormatCount = (prefer_rgba8) ? 2 : 4;
		return VK_SUCCESS;
	}
	*pSurfaceFormatCount = prefer_rgba8 ? 2 : 4;
	pSurfaceFormats[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	pSurfaceFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	pSurfaceFormats[1].format = VK_FORMAT_R8G8B8A8_SRGB;
	pSurfaceFormats[1].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	
	if (!prefer_rgba8) {
		pSurfaceFormats[2].format = VK_FORMAT_B8G8R8A8_UNORM;
		pSurfaceFormats[2].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		pSurfaceFormats[3].format = VK_FORMAT_B8G8R8A8_SRGB;
		pSurfaceFormats[3].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	}
	return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceSurfaceInfo2KHR *pInfo, uint32_t* pSurfaceFormatCount, VkSurfaceFormat2KHR* pSurfaceFormats)
{
	if (pSurfaceFormats == nullptr) {
		*pSurfaceFormatCount = (prefer_rgba8) ? 2 : 4;
		return VK_SUCCESS;
	}
	*pSurfaceFormatCount = (prefer_rgba8) ? 2 : 4;
	pSurfaceFormats[0].surfaceFormat.format = VK_FORMAT_R8G8B8A8_UNORM;
	pSurfaceFormats[0].surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	pSurfaceFormats[1].surfaceFormat.format = VK_FORMAT_R8G8B8A8_SRGB;
	pSurfaceFormats[1].surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

	if (!prefer_rgba8) {
		pSurfaceFormats[2].surfaceFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
		pSurfaceFormats[2].surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		pSurfaceFormats[3].surfaceFormat.format = VK_FORMAT_B8G8R8A8_SRGB;
		pSurfaceFormats[3].surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	}
	return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfacePresentModeCount, VkPresentModeKHR* pPresentModes)
{
	if (pPresentModes == nullptr) {
		if (pSurfacePresentModeCount) *pSurfacePresentModeCount = 2;
		return VK_SUCCESS;
	}
	uint32_t incomingCount = *pSurfacePresentModeCount;
	if (incomingCount >= 1) pPresentModes[0] = VK_PRESENT_MODE_FIFO_KHR;
	if (incomingCount >= 2) pPresentModes[1] = VK_PRESENT_MODE_IMMEDIATE_KHR;
	*pSurfacePresentModeCount = (incomingCount > 2) ? 2 : incomingCount;
	return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator)
{
	VK_UNWRAP_NON_DISPATCHABLE_HANDLE(surface, struct fake_surface, fake_surface)
	if (!fake_surface) return;
	scoped_lock l(global_lock);
	close(fake_surface->native_renderer_fd);
	free(fake_surface);
}

// --- [Swapchain Runtime Processing Engine] ---

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
	Logger::log("info", "[CreateSwapchain] === New Swapchain Request ===");
	auto dev = deviceDispatch[GetKey(device)];                              
	VkLayerDispatchTable table = dev->table;

	VkSwapchainCreateInfoKHR modifiedCreateInfo = *pCreateInfo;
	uint32_t origRequestedCount = pCreateInfo->minImageCount;

	if (modifiedCreateInfo.minImageCount < 3) {
		modifiedCreateInfo.minImageCount = 3; 
	}

	struct fake_swapchain *swapchain = (struct fake_swapchain *)calloc(1, sizeof(struct fake_swapchain));
	if (!swapchain) return VK_ERROR_OUT_OF_HOST_MEMORY;
    
	swapchain->loader_magic = ICD_LOADER_MAGIC;
	swapchain->obj_type = VK_OBJECT_TYPE_SWAPCHAIN_KHR;
	swapchain->wsi_device = device;
	swapchain->wait_for_present = true;
	swapchain->imageCount = modifiedCreateInfo.minImageCount;
	swapchain->requestedImageCount = origRequestedCount;
	swapchain->format = modifiedCreateInfo.imageFormat;
	swapchain->extent = modifiedCreateInfo.imageExtent;
	swapchain->presentMode = modifiedCreateInfo.presentMode;
	swapchain->device = dev;

	VK_UNWRAP_NON_DISPATCHABLE_HANDLE(modifiedCreateInfo.surface, struct fake_surface , fake_surface);
	if (fake_surface == nullptr) {
		free(swapchain); 
		return VK_ERROR_SURFACE_LOST_KHR; 
	}

	swapchain->surface = fake_surface;
	swapchain->currentImage = 0;
	swapchain->id = id.generate();
	swapchain->images.resize(swapchain->imageCount);
	
	int request_code = 1;
	write(fake_surface->native_renderer_fd, &request_code, 4);
	write(fake_surface->native_renderer_fd, &swapchain->id, 1);
	write(fake_surface->native_renderer_fd, &swapchain->imageCount, 4);
	
	for (uint32_t index = 0; index < swapchain->imageCount; index++) {
		VkResult result;
		int ret;
		auto fake_image = std::make_shared<struct fake_swapchain_image>();
		fake_image->width = swapchain->extent.width;
		fake_image->height = swapchain->extent.height;

		VkExternalMemoryImageCreateInfo externalCreateInfo{};
		externalCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		externalCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

		VkImageCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		createInfo.pNext = &externalCreateInfo;
		if (modifiedCreateInfo.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
			createInfo.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
		
		createInfo.imageType = VK_IMAGE_TYPE_2D;
		createInfo.format = swapchain->format;
		createInfo.extent = {fake_image->width, fake_image->height, 1};
		createInfo.mipLevels = 1;
		createInfo.arrayLayers = 1;
		createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		createInfo.usage = modifiedCreateInfo.imageUsage;
		createInfo.sharingMode = modifiedCreateInfo.imageSharingMode;	
		createInfo.queueFamilyIndexCount = modifiedCreateInfo.queueFamilyIndexCount;
		createInfo.pQueueFamilyIndices = modifiedCreateInfo.pQueueFamilyIndices;
		createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		result = table.CreateImage(device, &createInfo, nullptr, &fake_image->handle);
		if (result != VK_SUCCESS) return result;

		AHardwareBuffer_Desc desc{};
		desc.width = swapchain->extent.width;
		desc.height = swapchain->extent.height;
		desc.format = to_ahardwarebuffer_format(swapchain->format);
		desc.layers = 1;
		desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;

		ret = AHardwareBuffer_allocate(&desc, &fake_image->ahb);
		if (ret != 0) return VK_ERROR_OUT_OF_HOST_MEMORY;

		VkAndroidHardwareBufferPropertiesANDROID ahbProps{};
		ahbProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
		
		if (table.GetAndroidHardwareBufferPropertiesANDROID != nullptr) {
			table.GetAndroidHardwareBufferPropertiesANDROID(device, fake_image->ahb, &ahbProps);
		} else {
			auto pfnGetAHBProps = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)table.GetDeviceProcAddr(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
			if (pfnGetAHBProps != nullptr) pfnGetAHBProps(device, fake_image->ahb, &ahbProps);
			else return VK_ERROR_INITIALIZATION_FAILED;
		}

		VkMemoryDedicatedAllocateInfo dedicatedAlloc{};
		dedicatedAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		dedicatedAlloc.image = fake_image->handle;
		
		VkImportAndroidHardwareBufferInfoANDROID importAHB{};
		importAHB.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
		importAHB.pNext = &dedicatedAlloc;
		importAHB.buffer = fake_image->ahb;

		VkMemoryAllocateInfo allocateInfo{};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.pNext = &importAHB;
		allocateInfo.allocationSize = ahbProps.allocationSize;
		allocateInfo.memoryTypeIndex = pick_memory_index(swapchain->surface->instance, dev->physical, ahbProps.memoryTypeBits);

		result = table.AllocateMemory(device, &allocateInfo, nullptr, &fake_image->memory);
		if (result != VK_SUCCESS) return result;
			
		result = table.BindImageMemory(device, fake_image->handle, fake_image->memory, 0);
		if (result != VK_SUCCESS) return result;

		ret = AHardwareBuffer_sendHandleToUnixSocket(fake_image->ahb, swapchain->surface->native_renderer_fd);
		if (ret != 0) return VK_ERROR_INITIALIZATION_FAILED;
	
		swapchain->images[index] = fake_image;
	}

	*pSwapchain = VK_WRAP_NON_DISPATCHABLE_HANDLE(VkSwapchainKHR, swapchain);
	return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages)
{
    VK_UNWRAP_NON_DISPATCHABLE_HANDLE(swapchain, struct fake_swapchain, fake_swapchain)
    if (fake_swapchain == nullptr) return VK_ERROR_OUT_OF_DATE_KHR;

    auto dev = deviceDispatch[GetKey(device)];
    return dev->table.GetSwapchainImagesKHR(device, fake_swapchain->real_handle, pSwapchainImageCount, pSwapchainImages);
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
		AHardwareBuffer_release(swapchain_image->ahb);
		dev->table.DestroyImage(device, swapchain_image->handle, nullptr);
	}
	fake_swapchain->images.clear();

	int request_code = 3;
	write(fake_swapchain->surface->native_renderer_fd, &request_code, 4);
	write(fake_swapchain->surface->native_renderer_fd, &fake_swapchain->id, 1);
	id.destroy(fake_swapchain->id);
	free(swapchain);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	auto q = queues[queue];
	if (!q || !q->device || !q->fence) return VK_ERROR_OUT_OF_DATE_KHR;

	VkFenceGetFdInfoKHR getFence{};
	getFence.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
	getFence.fence = q->fence->handle;
	getFence.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	submitInfo.pWaitDstStageMask = waitStages.data();
	submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
	submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;

	q->device->table.QueueSubmit(q->handle, 1, &submitInfo, q->fence->handle);
	q->device->table.GetFenceFdKHR(q->device->handle, &getFence, &q->fence->sync_fd);

	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
		VK_UNWRAP_NON_DISPATCHABLE_HANDLE(pPresentInfo->pSwapchains[i], struct fake_swapchain, fake_swapchain)
		if (!fake_swapchain || !fake_swapchain->surface) continue;

		int request_code = 2;
		int index = pPresentInfo->pImageIndices[i];
		int fence = q->fence->sync_fd;
		
		write(fake_swapchain->surface->native_renderer_fd, &request_code, 4);
		write(fake_swapchain->surface->native_renderer_fd, &fake_swapchain->id, 1);
		write(fake_swapchain->surface->native_renderer_fd, &index, 4);
		sendFD(fake_swapchain->surface->native_renderer_fd, fence);
	}

	if (q->fence->sync_fd >= 0) {
		close(q->fence->sync_fd);
		q->fence->sync_fd = -1; 
	}
	return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL
DisplayX_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	auto dev = deviceDispatch[GetKey(device)];
	if (!device || !dev) return;

	scoped_lock l(global_lock);
	dev->table.DeviceWaitIdle(device);
	for (auto it = queues.begin(); it != queues.end();) {
		dev->table.DestroyFence(device, it->second->fence->handle, nullptr);
		it = queues.erase(it);
	}
	dev->table.DestroyDevice(device, pAllocator);
	deviceDispatch.erase(GetKey(device));
}

// --- [ProcAddr Dispatch Tables (Cleaned)] ---

VK_LAYER_EXPORT VkResult VKAPI_CALL
DisplayX_GetPhysicalDeviceSurfaceCapabilities2EXT(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, void* pSurfaceCapabilities) {
    return DisplayX_GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, (VkSurfaceCapabilitiesKHR*)pSurfaceCapabilities);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
DisplayX_GetDeviceProcAddr(VkDevice device, const char *pName)
{   
    // 🌟 매크로(##) 제거 완료: 명시적 함수 포인터 주소 전달
    if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return (PFN_vkVoidFunction)&DisplayX_GetPhysicalDeviceSurfaceCapabilitiesKHR;
    }
    if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2KHR")) {
        return (PFN_vkVoidFunction)&DisplayX_GetPhysicalDeviceSurfaceCapabilities2KHR;
    }
    if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2EXT")) {
        return (PFN_vkVoidFunction)&DisplayX_GetPhysicalDeviceSurfaceCapabilities2EXT;
    }

    GETPROCADDR(DestroyDevice);
    GETPROCADDR(CreateDevice);
    GETPROCADDR(CreateSwapchainKHR);
    GETPROCADDR(DestroySwapchainKHR);
    GETPROCADDR(GetSwapchainImagesKHR);
    GETPROCADDR(AcquireNextImageKHR);
    GETPROCADDR(AcquireNextImage2KHR);
    GETPROCADDR(GetDeviceQueue);
    GETPROCADDR(GetDeviceQueue2);
    GETPROCADDR(QueuePresentKHR);
    GETPROCADDR(WaitForPresentKHR);

    {
        scoped_lock l(global_lock);
        auto dev = deviceDispatch[GetKey(device)];
        return dev->table.GetDeviceProcAddr(device, pName);
    }
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
DisplayX_GetInstanceProcAddr(VkInstance instance, const char *pName)
{   
    // 🌟 매크로(##) 제거 완료
    if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return (PFN_vkVoidFunction)&DisplayX_GetPhysicalDeviceSurfaceCapabilitiesKHR;
    }
    if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2KHR")) {
        return (PFN_vkVoidFunction)&DisplayX_GetPhysicalDeviceSurfaceCapabilities2KHR;
    }
    if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2EXT")) {
        return (PFN_vkVoidFunction)&DisplayX_GetPhysicalDeviceSurfaceCapabilities2EXT;
    }

    GETPROCADDR(CreateInstance);
    GETPROCADDR(DestroyInstance);
    GETPROCADDR(CreateDevice);
    GETPROCADDR(CreateXcbSurfaceKHR);
    GETPROCADDR(CreateXlibSurfaceKHR);
    GETPROCADDR(GetPhysicalDeviceSurfaceSupportKHR);
    GETPROCADDR(GetPhysicalDeviceSurfaceFormatsKHR);
    GETPROCADDR(GetPhysicalDeviceSurfaceFormats2KHR);
    GETPROCADDR(DestroySurfaceKHR);
    GETPROCADDR(GetPhysicalDeviceSurfacePresentModesKHR);

    {
        scoped_lock l(global_lock);
        VkLayerInstanceDispatchTable table = instanceDispatch[GetKey(instance)];
        return table.GetInstanceProcAddr(instance, pName);
    }
}
