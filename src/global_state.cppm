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
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>

export module global_state;
import rcu;

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

export template <typename T>
void modify(
	std::atomic<std::shared_ptr<const T>> &value,
	std::type_identity_t<std::function<void(T &)>> fn
) {
	std::shared_ptr<const T> current = value.load();
	while (true) {
		// copy
		std::shared_ptr<T> next = std::make_shared<T>(*current);
		// modify
		fn(*next);
		// store
		std::shared_ptr<const T> expected = current;
		if (value.compare_exchange_weak(expected, std::move(next))) {
			break;
		}

		current = expected;
	}
}

using fn_map = std::map<void *, vk::detail::DispatchLoaderDynamic>;

export class globals {
	struct swapchain_data {
		vk::Extent2D extent;
		std::vector<vk::Image> images;
		std::vector<vk::UniqueImageView> image_views;
		vk::Format image_format;
	};
	using SwapchainMap = std::map<vk::SwapchainKHR, std::shared_ptr<const swapchain_data>>;
	public:

	globals() : l(init_logger()) {
		ImGui::SetCurrentContext(imgui_ctx.get());
	}

	~globals() {
		l.trace(__func__);

		auto sw_data = _swapchain_data.read();
		for (const auto &[swapchain, data] : *sw_data) {
			l.info(
				"Swapchain: {} w: {} h: {}",
				reinterpret_cast<uint64_t>(static_cast<VkSwapchainKHR>(swapchain)),
				data->extent.width,
				data->extent.height
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

	std::chrono::time_point<std::chrono::high_resolution_clock> last_frame = std::chrono::high_resolution_clock::now();

	std::shared_mutex init_info_lock;
	ImGui_ImplVulkan_InitInfo init_info = {};
	// Must outlive init_info and be written only under init_info_lock.
	vk::Format swapchain_color_format = vk::Format::eUndefined;

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
		logger.flush_on(spdlog::level::trace);

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
			if (init_info.UseDynamicRendering) {
				const auto &pr = init_info.PipelineInfoMain.PipelineRenderingCreateInfo;
				if (pr.colorAttachmentCount == 0 || pr.pColorAttachmentFormats == nullptr) {
					l.warn("init_imgui: PipelineRenderingCreateInfo not set");
					ready = false;
				}
			} else {
				if (init_info.PipelineInfoMain.RenderPass == VK_NULL_HANDLE) {
					l.warn("init_imgui: RenderPass null");
					ready = false;
				}
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
	void init_swapchain_data(vk::Device device, vk::SwapchainKHR sw, const vk::SwapchainCreateInfoKHR &create_info) {
		l.debug(__func__);
		const auto format = create_info.imageFormat;
		const auto extent = create_info.imageExtent;

		
		const auto &dld = device_dispatch.read()->at(get_key(device));
		auto images = device.getSwapchainImagesKHR(sw, dld);
		if (!images) {
			l.error("Can't get swapchain images: {}", vk::to_string(images.error()));
			return;
		}
		auto images_value = std::move(*images);
		update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
			info.ImageCount = images_value.size();
		});
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
		image_views.reserve(images_value.size());
		for (const auto &image : images_value) {
			info.image = image;
			auto image_view = device.createImageViewUnique(info, nullptr, dld);
			if (!image_view) {
				l.error("Can't create image view: {}", vk::to_string(image_view.error()));
				return;
			}
			image_views.push_back(std::move(image_view.value()));
		}
		_swapchain_data.mutate([&](SwapchainMap &sw_data) -> void {
			auto entry = std::make_shared<const swapchain_data>(swapchain_data{
				.extent = extent,
				.images = std::move(images_value),
				.image_views = std::move(image_views),
				.image_format = format,
			});
			sw_data.emplace(
				sw, entry
			);
		});
		l.debug("fine");
	}

	void remove_swapchain_data(VkSwapchainKHR sw) {
		_swapchain_data.mutate([&](SwapchainMap &sw_data) -> void {
			sw_data.erase(sw);
		});
	}

	rcu<std::map<void *, vk::Instance>> instance_map;
	rcu<std::map<void *, vk::detail::DispatchLoaderDynamic>> instance_dispatch;
	rcu<std::map<void *, vk::detail::DispatchLoaderDynamic>> device_dispatch;
	private:

	rcu<SwapchainMap> _swapchain_data;
};
