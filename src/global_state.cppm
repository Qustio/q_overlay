module;

#include <chrono>
#include <cstdint>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
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
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_core.h>

export module global_state;

// use the loader's dispatch table pointer as a key for dispatch map lookups
export template <typename DispatchableType>
auto get_key(const DispatchableType &inst) -> void * {
	if constexpr (requires { typename DispatchableType::NativeType; }) {
		auto ctype = static_cast<typename DispatchableType::NativeType>(inst);
		return *reinterpret_cast<void **>(ctype);
	} else {
		return *reinterpret_cast<void **>(inst);
	}
}

export class globals {
	struct swapchain_data {
		vk::Extent2D extent;
		std::vector<vk::Image> images;
		std::vector<vk::UniqueImageView> image_views;
		vk::Format image_format;
	};
	public:

	globals() : l(init_logger()) {
		ImGui::SetCurrentContext(imgui_ctx.get());
	}

	~globals() {
		l.trace(__func__);

		std::shared_lock lock(sw_lock);
		for (const auto &[swapchain, data] : _swapchain_data) {
			l.info(
				"Swapchain: {} w: {} h: {}",
				reinterpret_cast<uint64_t>(static_cast<VkSwapchainKHR>(swapchain)),
				data.extent.width,
				data.extent.height
			);
		}
		l.info(count.load());
		l.info("Exit");
		l.flush();
	}

	// imgui context
	std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> imgui_ctx{
		ImGui::CreateContext(),
		&ImGui::DestroyContext
	};
	std::atomic_bool imgui_rendered{false};
	bool imgui_initialized{false};
	auto shutdown_imgui() -> void {
		if (imgui_initialized) {
			ImGui_ImplVulkan_Shutdown();
			imgui_initialized = false;
			imgui_rendered.store(false);
		}
	}

	auto imgui() const -> ImDrawData * {
		if (!imgui_initialized) {
			return nullptr;
		}
		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();

		ImGui::Begin("q_overlay", nullptr);
		ImGui::Text("%.6f", frametime);
		ImGui::End();
		ImGui::ShowDemoWindow(nullptr);

		ImGui::Render();
		return ImGui::GetDrawData();
	}

	spdlog::logger l;
	double frametime = 0;
	std::atomic_size_t count{0};
	std::shared_mutex sw_lock;
	std::map<void *, vk::Instance> instance_map;
	std::map<void *, vk::detail::DispatchLoaderDynamic> instance_dispatch;
	std::map<void *, vk::detail::DispatchLoaderDynamic> device_dispatch;
	std::shared_mutex global_lock;
	std::chrono::time_point<std::chrono::high_resolution_clock> last_frame = std::chrono::high_resolution_clock::now();

	std::shared_mutex init_info_lock;
	ImGui_ImplVulkan_InitInfo init_info = {};

	vk::DescriptorPool descriptor_pool;
	vk::RenderPass render_pass;
	static auto init_logger() -> spdlog::logger {
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
	template <typename F>
	requires std::invocable<F, ImGui_ImplVulkan_InitInfo &> && std::is_void_v<std::invoke_result_t<F, ImGui_ImplVulkan_InitInfo &>>
	auto update_imgui_init_info(F &&func) -> void {
		{
			std::unique_lock lock{init_info_lock};
			std::forward<F>(func)(init_info);
		}
		init_imgui();
	}
	auto init_imgui() -> void {
		if (imgui_rendered.load()) {
			return;
		}
		{
			std::shared_lock lock{init_info_lock};
			bool ready = true;
			if (init_info.Instance == VK_NULL_HANDLE) {
				l.warn("init_imgui: Instance null");
				ready = false;
			}
			if (init_info.PhysicalDevice == VK_NULL_HANDLE) {
				l.warn("init_imgui: PhysicalDevice null");
				ready = false;
			}
			if (init_info.Device == VK_NULL_HANDLE) {
				l.warn("init_imgui: Device null");
				ready = false;
			}
			if (init_info.Queue == VK_NULL_HANDLE) {
				l.warn("init_imgui: Queue null");
				ready = false;
			}
			if (init_info.ImageCount == 0) {
				l.warn("init_imgui: ImageCount 0");
				ready = false;
			}
			if (init_info.PipelineInfoMain.RenderPass == VK_NULL_HANDLE) {
				l.warn("init_imgui: RenderPass null");
				ready = false;
			}
			if (!ready) {
				return;
			}
		}
		if (!imgui_rendered.exchange(true)) {
			l.info("Calling ImGui_ImplVulkan_Init RenderPass: {:x}", (uint64_t)init_info.PipelineInfoMain.RenderPass);
			l.info("Calling ImGui_ImplVulkan_Init...");
			auto res = ImGui_ImplVulkan_Init(&init_info);
			l.info("Imgui init result: {}", res);
			l.flush();
			if (res) {
				imgui_initialized = true;
			}
		}
	}
	void init_swapchain_data(vk::Device device, vk::SwapchainKHR sw, vk::Format format, vk::Extent2D extent) {
		std::shared_lock lock(global_lock);
		const auto &dld = device_dispatch[get_key(device)];
		auto [result, images] = device.getSwapchainImagesKHR(sw, dld);
		if (result != vk::Result::eSuccess) {
			l.error("Can't get swapchain images");
			return;
		}
		vk::ImageViewCreateInfo info{};
		info.viewType = vk::ImageViewType::e2D;
		info.format = format;
		info.subresourceRange = {
			vk::ImageAspectFlagBits::eColor,
			0,
			1, // mip
			0,
			1 // array
		};
		std::vector<vk::UniqueImageView> image_views{};
		image_views.reserve(images.size());
		for (const auto &image : images) {
			info.image = image;
			auto [result, image_view] = device.createImageViewUnique(info, nullptr);
			if (result != vk::Result::eSuccess) {
				l.error("Can't create image view");
				return;
			}
			image_views.push_back(std::move(image_view));
		}
		_swapchain_data.emplace(
			sw,
			swapchain_data{
				.extent = extent,
				.images = std::move(images),
				.image_views = std::move(image_views),
				.image_format = format,
			}
		);
	}
	void remove_swapchain_data(VkSwapchainKHR sw) {
		_swapchain_data.erase(sw);
	}
	private:

	std::map<vk::SwapchainKHR, swapchain_data> _swapchain_data;
};
