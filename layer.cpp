#include "layer.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <chrono>
#include <ratio>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan_core.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>

using scoped_lock = std::unique_lock<std::mutex>;

static std::mutex global_lock;
static auto last_frame = std::chrono::high_resolution_clock::now();
static uint32_t vulkan_api_version;

// use the loader's dispatch table pointer as a key for dispatch map lookups
template<typename DispatchableType>
void *GetKey(DispatchableType inst)
{
	return *(void **)inst;
}

int aboba(int a) {
	return a*a;
}

// layer book-keeping information, to store dispatch tables by key
std::map<void *, VkuInstanceDispatchTable> instance_dispatch;
std::map<void *, VkuDeviceDispatchTable> device_dispatch;

// logger
std::shared_ptr<spdlog::logger> global_logger = [](){
	auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("q_overlay.log", true);
    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

	file_sink->set_level(spdlog::level::trace);
	stdout_sink->set_level(spdlog::level::debug);

	auto logger = std::make_shared<spdlog::logger>("q_overlay", spdlog::sinks_init_list{file_sink, stdout_sink});
	logger->set_level(spdlog::level::trace);
	logger->flush_on(spdlog::level::debug);
	std::vector<int> vvv{};

	return logger;
}();

static void init_logger() {
	spdlog::set_default_logger(global_logger);
	spdlog::set_level(spdlog::level::trace);
	spdlog::flush_on(spdlog::level::trace);
}

static void init_imgui_vulkan(VkDevice pDevice, uint32_t api_version) {
	ImGui_ImplVulkan_LoadFunctions(api_version, [](const char* name, void* user_data) {
		auto device = reinterpret_cast<VkDevice>(user_data);
		scoped_lock l(global_lock);
		return device_dispatch[GetKey(device)].GetDeviceProcAddr(device, name);
	}, (void *)pDevice);
}

static VkResult VKAPI_CALL Q_CreateInstance(
    const VkInstanceCreateInfo  *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkInstance                  *pInstance
) {
	spdlog::trace("Q_CreateInstance called");
    VkLayerInstanceCreateInfo *layerCreateInfo = reinterpret_cast<VkLayerInstanceCreateInfo *>(const_cast<void *>(pCreateInfo->pNext));
    while (
		layerCreateInfo &&
		(
			layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
			layerCreateInfo->function != VK_LAYER_LINK_INFO
		)
	) {
		layerCreateInfo = reinterpret_cast<VkLayerInstanceCreateInfo *>(const_cast<void *>(layerCreateInfo->pNext));
    }
    if (!layerCreateInfo)
		return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	// advance chain
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    PFN_vkCreateInstance fpCreate = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!fpCreate)
		return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = fpCreate(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS)
		return result;

	VkuInstanceDispatchTable dispatchTable;
	dispatchTable.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gipa(*pInstance, "vkGetInstanceProcAddr");
	dispatchTable.DestroyInstance = (PFN_vkDestroyInstance)gipa(*pInstance, "vkDestroyInstance");
	//dispatchTable.EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)gipa(*pInstance, "vkEnumerateDeviceExtensionProperties");

	{
		scoped_lock l(global_lock);
		instance_dispatch[GetKey(*pInstance)] = dispatchTable;
	}

	// fill api version
	vulkan_api_version = pCreateInfo->pApplicationInfo->apiVersion;

    return VK_SUCCESS;
}

static VkResult VKAPI_CALL Q_CreateDevice(
    VkPhysicalDevice             physicalDevice,
    const VkDeviceCreateInfo    *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice                    *pDevice)
{
	spdlog::trace("Q_CreateDevice called");
    VkLayerDeviceCreateInfo *layerCreateInfo = reinterpret_cast<VkLayerDeviceCreateInfo *>(const_cast<void *>(pCreateInfo->pNext));
	while (
		layerCreateInfo &&
		(
			layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
			layerCreateInfo->function != VK_LAYER_LINK_INFO
		)
	) {
		layerCreateInfo = reinterpret_cast<VkLayerDeviceCreateInfo *>(const_cast<void *>(layerCreateInfo->pNext));
    }
    if (!layerCreateInfo)
		return VK_ERROR_INITIALIZATION_FAILED;

	PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;

	// advance chain
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    auto fpCreate = reinterpret_cast<PFN_vkCreateDevice>(gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!fpCreate)
		return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = fpCreate(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS)
		return result;

    VkuDeviceDispatchTable dispatchTable;
	dispatchTable.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gdpa(*pDevice, "vkGetDeviceProcAddr");
	dispatchTable.DestroyDevice = (PFN_vkDestroyDevice)gdpa(*pDevice, "vkDestroyDevice");
	dispatchTable.QueueSubmit = (PFN_vkQueueSubmit)gdpa(*pDevice, "vkQueueSubmit");
	dispatchTable.QueuePresentKHR = (PFN_vkQueuePresentKHR)gdpa(*pDevice, "vkQueuePresentKHR");
	//dispatchTable.CmdDraw = (PFN_vkCmdDraw)gdpa(*pDevice, "vkCmdDraw");
	//dispatchTable.CmdDrawIndexed = (PFN_vkCmdDrawIndexed)gdpa(*pDevice, "vkCmdDrawIndexed");
	//dispatchTable.EndCommandBuffer = (PFN_vkEndCommandBuffer)gdpa(*pDevice, "vkEndCommandBuffer");

	// store the table by key
	{
		scoped_lock l(global_lock);
		device_dispatch[GetKey(*pDevice)] = dispatchTable;
	}

	// init imgui vulkan function loader
	init_imgui_vulkan(*pDevice, vulkan_api_version);

	// ImGui_ImplVulkan_InitInfo init_info = {};
    // init_info.Instance       = 
    // init_info.PhysicalDevice = physicalDevice;
    // init_info.Device         = *pDevice;
    // init_info.Queue          = 
    // // ...
    // ImGui_ImplVulkan_Init(&init_info);

	return VK_SUCCESS;
}

static void VKAPI_CALL Q_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
	scoped_lock l(global_lock);
	auto DestroyDevice = device_dispatch[GetKey(device)].DestroyDevice;
	device_dispatch.erase(GetKey(device));
	l.unlock();
	DestroyDevice(device, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL Q_QueueSubmit(
    VkQueue             queue,
    uint32_t            submitCount,
    const VkSubmitInfo *pSubmits,
    VkFence             fence)
{
	spdlog::trace("Q_QueueSubmit");
	scoped_lock l(global_lock);
	auto QueueSubmit = device_dispatch[GetKey(queue)].QueueSubmit;
	l.unlock();
	return QueueSubmit(queue, submitCount, pSubmits, fence);
}

VkResult VKAPI_CALL Q_QueuePresentKHR(
    VkQueue                 queue,
    const VkPresentInfoKHR *pPresentInfo)
{

	auto now = std::chrono::high_resolution_clock::now();
	auto elapsed = std::chrono::duration<double, std::milli>(now - last_frame);
	auto d = elapsed.count();
	last_frame = now;
	spdlog::info(
		"frame time: {:.2f}ms ({:.1f} FPS)",
        elapsed.count(),
        1000.0 / elapsed.count()
	);
	for (int i = 0; i < pPresentInfo->swapchainCount; i++){
		spdlog::info(
			"swapchain: [{}]: {}",
			i,
			(uint64_t)pPresentInfo->pSwapchains[i]
		);
	}

	spdlog::trace("Q_QueuePresentKHR");
	scoped_lock l(global_lock);
	auto QueuePresent = device_dispatch[GetKey(queue)].QueuePresentKHR;
	l.unlock();
	return QueuePresent(queue, pPresentInfo);
}

extern "C" {
	VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Q_GetDeviceProcAddr(VkDevice device, const char *pName) {
		if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_GetDeviceProcAddr);
		if (strcmp(pName, "vkCreateDevice") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_CreateDevice);
		if (strcmp(pName, "vkDestroyDevice")  == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_DestroyDevice);
		if (strcmp(pName, "vkQueueSubmit") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_QueueSubmit);
		if (strcmp(pName, "vkQueuePresentKHR") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_QueuePresentKHR);
		{
			scoped_lock l(global_lock);
			return device_dispatch[GetKey(device)].GetDeviceProcAddr(device, pName);
		}
	}

	VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Q_GetInstanceProcAddr(VkInstance instance, const char *pName) {
		if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_GetInstanceProcAddr);
		if (strcmp(pName, "vkCreateInstance") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_CreateInstance);
		if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_GetDeviceProcAddr);
		if (strcmp(pName, "vkCreateDevice") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_CreateDevice);
		if (strcmp(pName, "vkDestroyDevice") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_DestroyDevice);
		if (strcmp(pName, "vkQueueSubmit") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_QueueSubmit);
		if (strcmp(pName, "vkQueuePresentKHR") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(Q_QueuePresentKHR);
		{
			scoped_lock l(global_lock);
			return instance_dispatch[GetKey(instance)].GetInstanceProcAddr(instance, pName);
		}
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct) {
		if (pVersionStruct->loaderLayerInterfaceVersion < 2)
			return VK_ERROR_INITIALIZATION_FAILED;

		pVersionStruct->loaderLayerInterfaceVersion    = 2;
		pVersionStruct->pfnGetInstanceProcAddr         = Q_GetInstanceProcAddr;
		pVersionStruct->pfnGetDeviceProcAddr           = Q_GetDeviceProcAddr;
		pVersionStruct->pfnGetPhysicalDeviceProcAddr   = nullptr;

		init_logger();

		return VK_SUCCESS;
	}
}


