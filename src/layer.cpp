#include <chrono>
#include <cstdint>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>
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

#include "src/global_state.cpp"

#undef EXPORT
#ifdef _WIN32
	#define EXPORT __declspec(dllexport)
#else
	#define EXPORT
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
	globals state;
	// https://github.com/ocornut/imgui/issues/4854#issuecomment-1783950012
	void init_imgui_vulkan(
		vk::Instance instance,
		vk::Device device,
		uint32_t api_version
	) {
		state.l.info(
			"Vulkan API version: {}.{}.{}",
			VK_API_VERSION_MAJOR(api_version),
			VK_API_VERSION_MINOR(api_version),
			VK_API_VERSION_PATCH(api_version)
		);
		struct loader_data {
			vk::Instance instance;
			vk::Device device;
		};
		loader_data loaders{
			.instance = instance,
			.device = device
		};
		auto res = ImGui_ImplVulkan_LoadFunctions(
			api_version,
			[](const char *name, void *user_data) -> PFN_vkVoidFunction {
				state.l.trace(name);

				auto *data = reinterpret_cast<loader_data *>(user_data);
				PFN_vkGetDeviceProcAddr device_func = state.device_dispatch.read()->at(get_key(data->device)).vkGetDeviceProcAddr;
				PFN_vkGetInstanceProcAddr instance_func = state.instance_dispatch.read()->at(get_key(data->instance)).vkGetInstanceProcAddr;
				auto device_addr = device_func(data->device, name);
				if (device_addr) {
					state.l.trace("device");
					return device_addr;
				}
				auto name_view = std::string_view{name};
				if (name_view.starts_with("vkCmdBeginRendering")
					|| name_view.starts_with("vkCmdEndRendering")
					|| name_view.starts_with("vkCmdBeginRenderingKHR")
					|| name_view.starts_with("vkCmdEndRenderingKHR")) {
					return nullptr;
				}
				auto instance_addr = instance_func(data->instance, name);
				if (instance_addr) {
					state.l.trace("instance");
					return instance_addr;
				}
				state.l.trace("unknown function");
				state.l.flush();
				return nullptr;
			},
			&loaders
		);
		state.l.info("ImGui_ImplVulkan_LoadFunctions returned: {}", res);
	}

	VKAPI_ATTR auto VKAPI_CALL
	Q_CreateInstance(
		const VkInstanceCreateInfo *pCreateInfo,
		const VkAllocationCallbacks *pAllocator,
		VkInstance *pInstance
	) -> VkResult {
		state.l.trace(__func__);

		auto *layerCreateInfo =
			reinterpret_cast<VkLayerInstanceCreateInfo *>(
				const_cast<void *>(pCreateInfo->pNext)
			);
		while (
			(layerCreateInfo != nullptr)
			&& (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
				|| layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
			layerCreateInfo = reinterpret_cast<VkLayerInstanceCreateInfo *>(
				const_cast<void *>(layerCreateInfo->pNext)
			);
		}
		if (layerCreateInfo == nullptr) {
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		PFN_vkGetInstanceProcAddr gipa =
			layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
		// advance chain
		layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

		auto fn_CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
			gipa(VK_NULL_HANDLE, "vkCreateInstance")
		);
		if (fn_CreateInstance == nullptr) {
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		VkResult result = fn_CreateInstance(pCreateInfo, pAllocator, pInstance);
		if (result != VK_SUCCESS) {
			return result;
		}

		vk::detail::DispatchLoaderDynamic dispatchTable{*pInstance, gipa};

		vk::Instance instance{*pInstance};
		state.instance_dispatch.mutate([&](auto &i_dld) {
			i_dld[get_key(instance)] = dispatchTable;
		});
		state.instance_map.mutate([&](auto &i_map) {
			i_map[get_key(instance)] = instance;
		});

		state.update_imgui_init_info([&](auto &info) -> void {
			info.Instance = *pInstance;
			info.UseDynamicRendering = true;
			info.ApiVersion = pCreateInfo->pApplicationInfo->apiVersion;
			info.DescriptorPoolSize = 64;
		});

		return VK_SUCCESS;
	}

	VKAPI_ATTR auto VKAPI_CALL
	Q_CreateDevice(
		VkPhysicalDevice physicalDevice,
		const VkDeviceCreateInfo *pCreateInfo,
		const VkAllocationCallbacks *pAllocator,
		VkDevice *pDevice
	) -> VkResult {
		state.l.trace(__func__);

		auto *layerCreateInfo =
			reinterpret_cast<VkLayerDeviceCreateInfo *>(
				const_cast<void *>(pCreateInfo->pNext)
			);
		while (
			(layerCreateInfo != nullptr)
			&& (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
				|| layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
			layerCreateInfo = reinterpret_cast<VkLayerDeviceCreateInfo *>(
				const_cast<void *>(layerCreateInfo->pNext)
			);
		}
		if (layerCreateInfo == nullptr) {
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		PFN_vkGetInstanceProcAddr gipa =
			layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
		PFN_vkGetDeviceProcAddr gdpa =
			layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;

		// advance chain
		layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

		auto fn_CreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
			gipa(VK_NULL_HANDLE, "vkCreateDevice")
		);
		if (fn_CreateDevice == nullptr) {
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		VkResult result =
			fn_CreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
		if (result != VK_SUCCESS) {
			return result;
		}

		vk::Instance instance = state.instance_map.read()->at(get_key(physicalDevice));
		vk::Device device{*pDevice};
		vk::detail::DispatchLoaderDynamic copy_dld = state.instance_dispatch.read()->at(get_key(physicalDevice));
		copy_dld.vkGetDeviceProcAddr = gdpa;
		copy_dld.init(device);
		state.device_dispatch.mutate([&](auto &d_dld) {
			d_dld[get_key(device)] = copy_dld;
		});

		uint32_t api;
		state.update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
			api = info.ApiVersion;
			info.PhysicalDevice = physicalDevice;
			info.Device = *pDevice;
			state.l.info("PhysicalDevice");
			state.l.info("Device");
		});
		init_imgui_vulkan(instance, device, api);

		return VK_SUCCESS;
	}

#ifdef VK_USE_PLATFORM_WIN32_KHR

	static LRESULT CALLBACK Q_HookedWndProc(
		HWND hwnd,
		UINT msg,
		WPARAM wParam,
		LPARAM lParam
	) {
		// Let ImGui see the message first.
		if (ImGui::GetCurrentContext() != nullptr) {
			ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

			// If ImGui currently wants the input, swallow it.
			ImGuiIO &io = ImGui::GetIO();
			switch (msg) {
				case WM_LBUTTONDOWN:
				case WM_LBUTTONUP:
				case WM_LBUTTONDBLCLK:
				case WM_RBUTTONDOWN:
				case WM_RBUTTONUP:
				case WM_RBUTTONDBLCLK:
				case WM_MBUTTONDOWN:
				case WM_MBUTTONUP:
				case WM_MBUTTONDBLCLK:
				case WM_XBUTTONDOWN:
				case WM_XBUTTONUP:
				case WM_MOUSEWHEEL:
				case WM_MOUSEHWHEEL:
					if (io.WantCaptureMouse) return 0;
					break;
				case WM_KEYDOWN:
				case WM_KEYUP:
				case WM_SYSKEYDOWN:
				case WM_SYSKEYUP:
				case WM_CHAR:
					if (io.WantCaptureKeyboard) return 0;
					break;
			}
		}

		// Forward to whatever the app originally installed.
		return CallWindowProcW(state.win32_hook.original_wndproc, hwnd, msg, wParam, lParam);
	}

	VKAPI_ATTR auto VKAPI_CALL
	Q_CreateWin32Surface(
		VkInstance instance,
		const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
		const VkAllocationCallbacks *pAllocator,
		VkSurfaceKHR *pSurface
	) -> VkResult {
		state.l.trace(__func__);

		PFN_vkCreateWin32SurfaceKHR func = state.instance_dispatch.read()->at(get_key(instance)).vkCreateWin32SurfaceKHR;

		HWND hwnd = pCreateInfo->hwnd;
		if (state.win32_hook.hwnd != hwnd) {
			ImGui_ImplWin32_Init(pCreateInfo->hwnd);
			state.win32_hook.hwnd = hwnd;
			state.win32_hook.original_wndproc = reinterpret_cast<WNDPROC>(
				SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Q_HookedWndProc))
			);

			state.l.trace("win32 wndproc subclassed for hwnd {}", static_cast<void *>(hwnd));
		}

		return func(instance, pCreateInfo, pAllocator, pSurface);
	}
#endif

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
	VKAPI_ATTR auto VKAPI_CALL
	Q_CreateWaylandSurface(
		VkInstance instance,
		const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
		const VkAllocationCallbacks *pAllocator,
		VkSurfaceKHR *pSurface
	) -> VkResult {
		state.l.debug(__func__);

		PFN_vkCreateWaylandSurfaceKHR func = state.instance_dispatch.read()->at(get_key(instance)).vkCreateWaylandSurfaceKHR;

		return func(instance, pCreateInfo, pAllocator, pSurface);
	}
#endif

	VKAPI_ATTR void VKAPI_CALL
	Q_GetDeviceQueue(
		VkDevice device,
		uint32_t queueFamilyIndex,
		uint32_t queueIndex,
		VkQueue *pQueue
	) {
		state.l.debug(__func__);

		PFN_vkGetDeviceQueue func = state.device_dispatch.read()->at(get_key(device)).vkGetDeviceQueue;

		func(device, queueFamilyIndex, queueIndex, pQueue);
		state.l.debug("Family: {} Queue: {}", queueFamilyIndex, queueIndex);
		if (queueFamilyIndex != 0 || queueIndex != 0) {
			return;
		}
		state.update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
			info.QueueFamily = queueFamilyIndex;
			info.Queue = *pQueue;
			state.l.info("Queue");
			state.l.info("QueueFamily");
		});
	}

	VKAPI_ATTR void VKAPI_CALL
	Q_GetDeviceQueue2(
		VkDevice device,
		const VkDeviceQueueInfo2 *pQueueInfo,
		VkQueue *pQueue
	) {
		state.l.debug(__func__);

		PFN_vkGetDeviceQueue2 func = state.device_dispatch.read()->at(get_key(device)).vkGetDeviceQueue2;

		func(device, pQueueInfo, pQueue);
		state.l.debug("Family: {} Queue: {}", pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex);
		if (pQueueInfo->queueFamilyIndex != 0 || pQueueInfo->queueIndex != 0) {
			return;
		}
		state.update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
			info.QueueFamily = pQueueInfo->queueFamilyIndex;
			info.Queue = *pQueue;
			state.l.info("Queue");
			state.l.info("QueueFamily");
		});
	}

	VKAPI_ATTR auto VKAPI_CALL
	Q_CreateRenderPass(
		VkDevice device,
		const VkRenderPassCreateInfo *pCreateInfo,
		const VkAllocationCallbacks *pAllocator,
		VkRenderPass *pRenderPass
	) -> VkResult {
		state.l.debug(__func__);

		PFN_vkCreateRenderPass func = state.device_dispatch.read()->at(get_key(device)).vkCreateRenderPass;

		auto result = func(device, pCreateInfo, pAllocator, pRenderPass);

		// g.create_render_pass(device, pCreateInfo->pAttachments->format);
		return result;
	}

	VKAPI_ATTR auto VKAPI_CALL
	Q_CreateSwapchain(
		VkDevice device,
		const VkSwapchainCreateInfoKHR *pCreateInfo,
		const VkAllocationCallbacks *pAllocator,
		VkSwapchainKHR *pSwapchain
	) -> VkResult {
		state.l.debug(__func__);

		PFN_vkCreateSwapchainKHR func = state.device_dispatch.read()->at(get_key(device)).vkCreateSwapchainKHR;

		auto result = func(device, pCreateInfo, pAllocator, pSwapchain);
		ImGui::GetIO().DisplaySize = ImVec2(
			static_cast<float>(pCreateInfo->imageExtent.width),
			static_cast<float>(pCreateInfo->imageExtent.height)
		);
		if (result != VK_SUCCESS) {
			return result;
		}
		state.init_swapchain_data(
			device,
			*pSwapchain,
			*pCreateInfo
		);
		state.update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
			state.swapchain_color_format = vk::Format{pCreateInfo->imageFormat};
			auto pci = vk::PipelineRenderingCreateInfo(
				0,							   // viewMask
				1,							   // colorAttachmentCount
				&state.swapchain_color_format, // pColorAttachmentFormats
				vk::Format::eUndefined,		   // depthAttachmentFormat
				vk::Format::eUndefined		   // stencilAttachmentFormat
			);
			info.MinImageCount = pCreateInfo->minImageCount;
			info.PipelineInfoMain.PipelineRenderingCreateInfo = pci;
			state.l.info("MinImageCount: {}", pCreateInfo->minImageCount);
		});
		return result;
	}

	VKAPI_ATTR void VKAPI_CALL
	Q_DestroySwapchain(
		VkDevice device,
		VkSwapchainKHR swapchain,
		const VkAllocationCallbacks *pAllocator
	) {
		state.l.debug(__func__);

		PFN_vkDestroySwapchainKHR func = state.device_dispatch.read()->at(get_key(device)).vkDestroySwapchainKHR;

		state.remove_swapchain_data(swapchain);
		func(device, swapchain, pAllocator);
	}

	VKAPI_ATTR void VKAPI_CALL
	Q_DestroyDevice(
		VkDevice device,
		const VkAllocationCallbacks *pAllocator
	) {
		state.l.trace(__func__);

		state.shutdown_imgui();

		PFN_vkDestroyDevice func = state.device_dispatch.read()->at(get_key(device)).vkDestroyDevice;

		state.device_dispatch.mutate([&](auto &d_dld) {
			d_dld.erase(get_key(device));
		});
		func(device, pAllocator);
	}

	VKAPI_ATTR auto VKAPI_CALL
	Q_QueueSubmit(
		VkQueue queue,
		uint32_t submitCount,
		const VkSubmitInfo *pSubmits,
		VkFence fence
	) -> VkResult {
		state.l.trace(__func__);

		PFN_vkQueueSubmit func = state.device_dispatch.read()->at(get_key(queue)).vkQueueSubmit;

		return func(queue, submitCount, pSubmits, fence);
	}

	VKAPI_ATTR auto VKAPI_CALL
	Q_QueuePresentKHR(
		VkQueue queue,
		const VkPresentInfoKHR *pPresentInfo
	) -> VkResult {
		state.l.trace(__func__);

		PFN_vkQueuePresentKHR func = state.device_dispatch.read()->at(get_key(queue)).vkQueuePresentKHR;

		auto result = func(queue, pPresentInfo);
		auto now = std::chrono::high_resolution_clock::now();
		auto elapsed = std::chrono::duration<double, std::milli>(now - state.last_frame);
		state.frametime = elapsed.count();
		state.last_frame = now;

		return result;
	}

	VKAPI_ATTR void VKAPI_CALL
	Q_CmdEndRenderPass(
		VkCommandBuffer commandBuffer
	) {
		state.l.trace(__func__);

		PFN_vkCmdEndRenderPass func = state.device_dispatch.read()->at(get_key(commandBuffer)).vkCmdEndRenderPass;

		// auto *data = state.imgui();
		// if (data != nullptr) {
		// 	ImGui_ImplVulkan_RenderDrawData(data, commandBuffer);
		// }
		func(commandBuffer);
	}

	VKAPI_ATTR void VKAPI_CALL
	Q_CmdEndRenderPass2(
		VkCommandBuffer commandBuffer,
		const VkSubpassEndInfo *pSubpassEndInfo
	) {
		state.l.trace(__func__);

		PFN_vkCmdEndRenderPass2 func = state.device_dispatch.read()->at(get_key(commandBuffer)).vkCmdEndRenderPass2;

		// auto *data = state.imgui();
		// if (data != nullptr) {
		// 	ImGui_ImplVulkan_RenderDrawData(data, commandBuffer);
		// }
		func(commandBuffer, pSubpassEndInfo);
	}

	VKAPI_ATTR void VKAPI_CALL
	Q_CmdEndRendering(
		VkCommandBuffer commandBuffer
	) {
		state.l.trace(__func__);

		PFN_vkCmdEndRendering func = state.device_dispatch.read()->at(get_key(commandBuffer)).vkCmdEndRendering;

		auto *data = state.imgui();
		if (data != nullptr) {
			ImGui_ImplVulkan_RenderDrawData(data, commandBuffer);
		}
		func(commandBuffer);
	}
} // namespace

extern "C" EXPORT VKAPI_ATTR auto VKAPI_CALL
Q_GetDeviceProcAddr(VkDevice device, const char *pName) -> PFN_vkVoidFunction;

extern "C" EXPORT VKAPI_ATTR auto VKAPI_CALL
Q_GetInstanceProcAddr(VkInstance instance, const char *pName) -> PFN_vkVoidFunction;

static const std::map<std::string_view, PFN_vkVoidFunction> device_functions{
	{"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&Q_GetDeviceProcAddr)},
	{"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateDevice)},
	{"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(&Q_DestroyDevice)},
	{"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(&Q_QueueSubmit)},
	{"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(&Q_QueuePresentKHR)},
	{"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateSwapchain)},
	{"vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&Q_DestroySwapchain)},
	{"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(&Q_GetDeviceQueue)},
	{"vkGetDeviceQueue2", reinterpret_cast<PFN_vkVoidFunction>(&Q_GetDeviceQueue2)},
	{"vkCreateRenderPass", reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateRenderPass)},
	{"vkCmdEndRenderPass", reinterpret_cast<PFN_vkVoidFunction>(&Q_CmdEndRenderPass)},
	{"vkCmdEndRenderPass2", reinterpret_cast<PFN_vkVoidFunction>(&Q_CmdEndRenderPass2)},
	{"vkCmdEndRenderPass2KHR", reinterpret_cast<PFN_vkVoidFunction>(&Q_CmdEndRenderPass2)},
	{"vkCmdEndRendering", reinterpret_cast<PFN_vkVoidFunction>(&Q_CmdEndRendering)},
	{"vkCmdEndRenderingKHR", reinterpret_cast<PFN_vkVoidFunction>(&Q_CmdEndRendering)},
};

static const std::map<std::string_view, PFN_vkVoidFunction> instance_functions = {
	{"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&Q_GetInstanceProcAddr)},
	{"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateInstance)},
#ifdef VK_USE_PLATFORM_WIN32_KHR
	{"vkCreateWin32SurfaceKHR", reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateWin32Surface)},
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
	{"vkCreateWaylandSurfaceKHR", reinterpret_cast<PFN_vkVoidFunction>(&Q_CreateWaylandSurface)},
#endif
};

extern "C" {
	EXPORT VKAPI_ATTR auto VKAPI_CALL
	Q_GetDeviceProcAddr(VkDevice device, const char *pName) -> PFN_vkVoidFunction {
		auto iter = device_functions.find(pName);
		if (iter != device_functions.end()) {
			return iter->second;
		}
		{
			PFN_vkGetDeviceProcAddr func = state.device_dispatch.read()->at(get_key(device)).vkGetDeviceProcAddr;
			return func(device, pName);
		}
	}

	EXPORT VKAPI_ATTR auto VKAPI_CALL
	Q_GetInstanceProcAddr(VkInstance instance, const char *pName) -> PFN_vkVoidFunction {
		auto iter = instance_functions.find(pName);
		if (iter != instance_functions.end()) {
			return iter->second;
		}
		iter = device_functions.find(pName);
		if (iter != device_functions.end()) {
			return iter->second;
		}
		{
			PFN_vkGetInstanceProcAddr func = state.instance_dispatch.read()->at(get_key(instance)).vkGetInstanceProcAddr;
			return func(instance, pName);
		}
	}

	VKAPI_ATTR auto VKAPI_CALL
	vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct) -> VkResult {
		if (pVersionStruct->loaderLayerInterfaceVersion < 2) {
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		pVersionStruct->loaderLayerInterfaceVersion = 2;
		pVersionStruct->pfnGetInstanceProcAddr = Q_GetInstanceProcAddr;
		pVersionStruct->pfnGetDeviceProcAddr = Q_GetDeviceProcAddr;
		pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
		return VK_SUCCESS;
	}
}
