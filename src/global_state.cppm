module;

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <spdlog/common.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
export module global_state;

// use the loader's dispatch table pointer as a key for dispatch map lookups
export template <typename DispatchableType>
void *GetKey(DispatchableType inst) {
	return *reinterpret_cast<void **>(inst);
}

export struct SwapchainData {
	uint32_t h;
	uint32_t w;
};

export struct globals {
	globals() : l(init_logger()) {
	}

	~globals() {
		for (const auto &[s, size] : swapchains) {
			l.info(
				"Swapchain: {} w: {} h: {}",
				reinterpret_cast<uint64_t>(static_cast<VkSwapchainKHR>(s)),
				size.h,
				size.w
			);
		}
		l.info(count.load());
		l.info("Exit");
		l.flush();
	}

	spdlog::logger l;
	std::atomic_size_t count{0};
	std::shared_mutex sw_lock;
	std::map<vk::SwapchainKHR, SwapchainData> swapchains;
	std::map<void *, VkuInstanceDispatchTable> instance_dispatch;
	std::map<void *, VkuDeviceDispatchTable> device_dispatch;
	std::shared_mutex global_lock;
	std::chrono::time_point<std::chrono::high_resolution_clock> last_frame = std::chrono::high_resolution_clock::now();
	ImGui_ImplVulkan_InitInfo init_info = {};

	auto init_logger() -> spdlog::logger {
		auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
			"q_overlay.log", true
		);
		auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

		file_sink->set_level(spdlog::level::trace);
		stdout_sink->set_level(spdlog::level::debug);

		auto logger = spdlog::logger("q_overlay", spdlog::sinks_init_list{file_sink, stdout_sink});
		logger.set_level(spdlog::level::trace);
		logger.flush_on(spdlog::level::debug);

		return logger;
	}
	auto init_imgui() -> void {
		ImGui_ImplVulkan_Init(&init_info);
	}
};
