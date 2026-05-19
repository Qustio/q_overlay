#include <chrono>
#include <cstdint>
#include <cstring>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <map>
#include <mutex>
#include <ratio>
#include <shared_mutex>
#include <spdlog/fmt/fmt.h>
#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

import global_state;

#undef EXPORT
#ifdef _WIN32
	#define EXPORT __declspec(dllexport)
#else
	#define EXPORT
#endif

static globals g;

// https://github.com/ocornut/imgui/issues/4854#issuecomment-1783950012
static void init_imgui_vulkan(VkDevice pDevice, uint32_t api_version) {
	auto res = ImGui_ImplVulkan_LoadFunctions(
		api_version,
		[](const char *name, void *user_data) -> PFN_vkVoidFunction {
		auto *device = reinterpret_cast<VkDevice>(user_data);
		std::shared_lock l(g.global_lock);
		PFN_vkVoidFunction device_addr = g.device_dispatch[GetKey(device)].GetDeviceProcAddr(
			device, name
		);
		if (device_addr) {
			return device_addr;
		}
		return g.instance_dispatch[GetKey(g.instance)].GetInstanceProcAddr(
			g.instance, name
		);
	},
		(void *)pDevice
	);
	g.l.info("ImGui_ImplVulkan_LoadFunctions result: {}", res);
}

static VkResult VKAPI_CALL Q_CreateInstance(
	const VkInstanceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkInstance *pInstance
) {
	g.l.trace("Q_CreateInstance called");
	g.l.flush();
	VkLayerInstanceCreateInfo *layerCreateInfo =
		reinterpret_cast<VkLayerInstanceCreateInfo *>(
			const_cast<void *>(pCreateInfo->pNext)
		);
	while (layerCreateInfo
		   && (layerCreateInfo->sType
				   != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
			   || layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
		layerCreateInfo = reinterpret_cast<VkLayerInstanceCreateInfo *>(
			const_cast<void *>(layerCreateInfo->pNext)
		);
	}
	if (!layerCreateInfo)
		return VK_ERROR_INITIALIZATION_FAILED;

	PFN_vkGetInstanceProcAddr gipa =
		layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	// advance chain
	layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

	PFN_vkCreateInstance fpCreate = reinterpret_cast<PFN_vkCreateInstance>(
		gipa(VK_NULL_HANDLE, "vkCreateInstance")
	);
	if (!fpCreate)
		return VK_ERROR_INITIALIZATION_FAILED;

	VkResult result = fpCreate(pCreateInfo, pAllocator, pInstance);
	if (result != VK_SUCCESS)
		return result;

	VkuInstanceDispatchTable dispatchTable{};
	vkuInitInstanceDispatchTable(
		*pInstance,
		&dispatchTable,
		gipa
	);

	{
		std::unique_lock l(g.global_lock);
		g.instance_dispatch[GetKey(*pInstance)] = dispatchTable;
		g.instance = *pInstance;
	}

	// init_imgui_vulkan(*pInstance, pCreateInfo->pApplicationInfo->apiVersion);
	g.update_imgui_init_info([&](auto &info) -> void {
		info.Instance = *pInstance;
		info.ApiVersion = pCreateInfo->pApplicationInfo->apiVersion;
	});

	return VK_SUCCESS;
}

static VkResult VKAPI_CALL Q_CreateDevice(
	VkPhysicalDevice physicalDevice,
	const VkDeviceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkDevice *pDevice
) {
	g.l.trace("Q_CreateDevice called");
	VkLayerDeviceCreateInfo *layerCreateInfo =
		reinterpret_cast<VkLayerDeviceCreateInfo *>(
			const_cast<void *>(pCreateInfo->pNext)
		);
	while (layerCreateInfo
		   && (layerCreateInfo->sType
				   != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
			   || layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
		layerCreateInfo = reinterpret_cast<VkLayerDeviceCreateInfo *>(
			const_cast<void *>(layerCreateInfo->pNext)
		);
	}
	if (!layerCreateInfo)
		return VK_ERROR_INITIALIZATION_FAILED;

	PFN_vkGetInstanceProcAddr gipa =
		layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr gdpa =
		layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;

	// advance chain
	layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

	auto fpCreate = reinterpret_cast<PFN_vkCreateDevice>(
		gipa(VK_NULL_HANDLE, "vkCreateDevice")
	);
	if (!fpCreate)
		return VK_ERROR_INITIALIZATION_FAILED;

	VkResult result =
		fpCreate(physicalDevice, pCreateInfo, pAllocator, pDevice);
	if (result != VK_SUCCESS)
		return result;

	VkuDeviceDispatchTable dispatchTable{};
	vkuInitDeviceDispatchTable(
		*pDevice,
		&dispatchTable,
		gdpa
	);

	// store the table by key
	{
		std::unique_lock l(g.global_lock);
		g.device_dispatch[GetKey(*pDevice)] = dispatchTable;
	}

	g.update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
		init_imgui_vulkan(*pDevice, info.ApiVersion);
		info.PhysicalDevice = physicalDevice;
		info.Device = *pDevice;
		g.l.info("PhysicalDevice");
		g.l.info("Device");
	});
	g.create_descriptor_pool(*pDevice);

	return VK_SUCCESS;
}

#ifdef VK_USE_PLATFORM_WIN32_KHR

static VKAPI_ATTR VkResult VKAPI_CALL Q_CreateWin32Surface(
	VkInstance instance,
	const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkSurfaceKHR *pSurface
) {
	g.l.trace("Q_CreateWin32Surface");
	std::shared_lock l(g.global_lock);
	auto CreateWin32SurfaceKHR = g.instance_dispatch[GetKey(instance)].CreateWin32SurfaceKHR;
	l.unlock();
	auto result = CreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
	return result;
}

#endif

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
static VKAPI_ATTR VkResult VKAPI_CALL Q_CreateWaylandSurface(
	VkInstance instance,
	const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkSurfaceKHR *pSurface
) {
	g.l.debug("Q_CreateWaylandSurface");
	std::shared_lock l(g.global_lock);
	auto CreateWaylandSurfaceKHR = g.instance_dispatch[GetKey(instance)].CreateWaylandSurfaceKHR;
	l.unlock();
	auto result = CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
	return result;
}
#endif

static void VKAPI_CALL Q_GetDeviceQueue(
	VkDevice device,
	uint32_t queueFamilyIndex,
	uint32_t queueIndex,
	VkQueue *pQueue
) {
	g.l.debug("Q_GetDeviceQueue");
	std::shared_lock l(g.global_lock);
	auto GetDeviceQueue = g.device_dispatch[GetKey(device)].GetDeviceQueue;
	l.unlock();
	GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
	g.l.debug("Family: {} Queue: {}", queueFamilyIndex, queueIndex);
	g.update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
		info.QueueFamily = queueFamilyIndex;
		info.Queue = *pQueue;
		g.l.info("Queue");
		g.l.info("QueueFamily");
	});
}

static VKAPI_ATTR VkResult VKAPI_CALL Q_CreateRenderPass(
	VkDevice device,
	const VkRenderPassCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkRenderPass *pRenderPass
) {
	g.l.debug("Q_CreateRenderPass");
	std::shared_lock l(g.global_lock);
	auto &dt = g.device_dispatch[GetKey(device)];
	l.unlock();
	auto result = dt.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
	g.update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
		info.PipelineInfoMain.RenderPass = *pRenderPass;
		info.PipelineInfoMain.Subpass = 0;
		g.l.info("PipelineInfoMain.RenderPass");
	});
	// g.create_render_pass(device, pCreateInfo->pAttachments->format);
	return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL Q_CreateSwapchain(
	VkDevice device,
	const VkSwapchainCreateInfoKHR *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkSwapchainKHR *pSwapchain
) {
	g.l.debug("Q_CreateSwapchain");
	std::shared_lock l(g.global_lock);
	auto &dt = g.device_dispatch[GetKey(device)];
	l.unlock();
	auto result = dt.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
	auto h = pCreateInfo->imageExtent.height;
	auto w = pCreateInfo->imageExtent.width;
	ImGui::GetIO().DisplaySize = ImVec2(
		static_cast<float>(h),
		static_cast<float>(w)
	);
	if (result != VK_SUCCESS) {
		return result;
	}
	uint32_t image_count = 0;
	auto r = dt.GetSwapchainImagesKHR(device, *pSwapchain, &image_count, nullptr);
	if (r != VK_SUCCESS) {
		return result;
	}
	g.update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
		info.MinImageCount = pCreateInfo->minImageCount;
		info.ImageCount = image_count;
		g.l.info("MinImageCount: {}", pCreateInfo->minImageCount);
		g.l.info("ImageCount: {}", image_count);
	});
	g.init_imgui();
	std::unique_lock swl(g.sw_lock);
	auto sw = vk::SwapchainKHR(*pSwapchain);
	auto res = g.swapchains.find(sw);
	if (res == g.swapchains.end()) {
		g.count++;
	}
	g.swapchains.emplace(sw, SwapchainData{h, w});
	return result;
}

static void VKAPI_CALL Q_DestroySwapchain(
	VkDevice device,
	VkSwapchainKHR swapchain,
	const VkAllocationCallbacks *pAllocator
) {
	g.l.debug("Q_DestroySwapchain");
	std::shared_lock l(g.global_lock);
	auto DestroySwapchain = g.device_dispatch[GetKey(device)].DestroySwapchainKHR;
	l.unlock();
	DestroySwapchain(device, swapchain, pAllocator);
	std::unique_lock swl(g.sw_lock);
	auto sw = vk::SwapchainKHR(swapchain);
	g.swapchains.erase(sw);
	g.count--;
}

static void VKAPI_CALL
Q_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
	std::unique_lock l(g.global_lock);
	auto DestroyDevice = g.device_dispatch[GetKey(device)].DestroyDevice;
	g.device_dispatch.erase(GetKey(device));
	l.unlock();
	DestroyDevice(device, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL Q_QueueSubmit(
	VkQueue queue,
	uint32_t submitCount,
	const VkSubmitInfo *pSubmits,
	VkFence fence
) {
	g.l.trace("Q_QueueSubmit");
	std::shared_lock l(g.global_lock);
	auto QueueSubmit = g.device_dispatch[GetKey(queue)].QueueSubmit;
	l.unlock();
	return QueueSubmit(queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
Q_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
	g.l.trace("Q_QueuePresentKHR");

	auto now = std::chrono::high_resolution_clock::now();
	auto elapsed = std::chrono::duration<double, std::milli>(now - g.last_frame);
	g.frametime = elapsed.count();
	g.last_frame = now;
	// g.l.info(
	// 	"frame time: {:.2f}ms ({:.1f} FPS)", elapsed.count(), 1000.0 / elapsed.count()
	// );
	// for (int i = 0; i < pPresentInfo->swapchainCount; i++) {
	// 	g.l.info(
	// 		"swapchain: [{}]: {}", i, (uint64_t)pPresentInfo->pSwapchains[i]
	// 	);
	// }
	std::shared_lock l(g.global_lock);
	auto QueuePresent = g.device_dispatch[GetKey(queue)].QueuePresentKHR;
	l.unlock();
	return QueuePresent(queue, pPresentInfo);
}

void VKAPI_CALL
Q_CmdEndRenderPass(VkCommandBuffer commandBuffer) {
	g.l.trace("Q_CmdEndRenderPass");

	std::shared_lock l(g.global_lock);
	auto &dt = g.device_dispatch[GetKey(commandBuffer)];
	l.unlock();

	auto *data = g.imgui();
	g.l.trace("data is null?: {}", data == nullptr);
	g.l.trace("data valid: {}", data->Valid);
	g.l.flush();
	ImGui_ImplVulkan_RenderDrawData(data, commandBuffer);

	dt.CmdEndRenderPass(commandBuffer);
}

extern "C" {
	EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
	Q_GetDeviceProcAddr(VkDevice device, const char *pName) {
		if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_GetDeviceProcAddr);
		if (strcmp(pName, "vkCreateDevice") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateDevice);
		if (strcmp(pName, "vkDestroyDevice") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_DestroyDevice);
		if (strcmp(pName, "vkQueueSubmit") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_QueueSubmit);
		if (strcmp(pName, "vkQueuePresentKHR") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_QueuePresentKHR);
		if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateSwapchain);
		if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_DestroySwapchain);
		if (strcmp(pName, "vkGetDeviceQueue") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_GetDeviceQueue);
		if (strcmp(pName, "vkCreateRenderPass") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateRenderPass);
		if (strcmp(pName, "vkCmdEndRenderPass") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_CmdEndRenderPass);
		{
			std::shared_lock l(g.global_lock);
			return g.device_dispatch[GetKey(device)].GetDeviceProcAddr(
				device, pName
			);
		}
	}

	EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
	Q_GetInstanceProcAddr(VkInstance instance, const char *pName) {
		if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_GetInstanceProcAddr);
		if (strcmp(pName, "vkCreateInstance") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateInstance);
		if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_GetDeviceProcAddr);
		if (strcmp(pName, "vkCreateDevice") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateDevice);
		if (strcmp(pName, "vkDestroyDevice") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_DestroyDevice);
		if (strcmp(pName, "vkQueueSubmit") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_QueueSubmit);
		if (strcmp(pName, "vkQueuePresentKHR") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_QueuePresentKHR);
		if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_DestroySwapchain);
		if (strcmp(pName, "vkGetDeviceQueue") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_GetDeviceQueue);
		if (strcmp(pName, "vkCreateRenderPass") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateRenderPass);
		if (strcmp(pName, "vkCmdEndRenderPass") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_CmdEndRenderPass);

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
		if (strcmp(pName, "vkCreateWaylandSurfaceKHR") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateWaylandSurface);
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
		if (strcmp(pName, "vkCreateWin32SurfaceKHR") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateWin32Surface);
#endif
		{
			std::shared_lock l(g.global_lock);
			return g.instance_dispatch[GetKey(instance)].GetInstanceProcAddr(
				instance, pName
			);
		}
	}

	VKAPI_ATTR VkResult VKAPI_CALL
	vkNegotiateLoaderLayerInterfaceVersion(
		VkNegotiateLayerInterface *pVersionStruct
	) {
		if (pVersionStruct->loaderLayerInterfaceVersion < 2)
			return VK_ERROR_INITIALIZATION_FAILED;

		pVersionStruct->loaderLayerInterfaceVersion = 2;
		pVersionStruct->pfnGetInstanceProcAddr = Q_GetInstanceProcAddr;
		pVersionStruct->pfnGetDeviceProcAddr = Q_GetDeviceProcAddr;
		pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

		return VK_SUCCESS;
	}
}
