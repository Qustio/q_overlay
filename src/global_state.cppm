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

export module global_state;

// use the loader's dispatch table pointer as a key for dispatch map lookups
export template <typename DispatchableType>
auto get_key(DispatchableType inst) -> void * {
	return *reinterpret_cast<void **>(inst);
}

export class globals {
	struct swapchain_data {
		uint32_t width;
		uint32_t height;
		std::vector<vk::Image> images;
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
				data.height,
				data.width
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

	VkInstance instance;

	spdlog::logger l;
	double frametime = 0;
	std::atomic_size_t count{0};
	std::shared_mutex sw_lock;
	std::map<void *, VkuInstanceDispatchTable> instance_dispatch;
	std::map<void *, VkuDeviceDispatchTable> device_dispatch;
	std::shared_mutex global_lock;
	std::chrono::time_point<std::chrono::high_resolution_clock> last_frame = std::chrono::high_resolution_clock::now();

	std::shared_mutex init_info_lock;
	ImGui_ImplVulkan_InitInfo init_info = {};

	vk::DescriptorPool descriptor_pool;
	vk::RenderPass render_pass;
	void create_descriptor_pool(VkDevice device) {
		update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
			info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
		});
		return;
		std::array<vk::DescriptorPoolSize, 2> pool_sizes{{{vk::DescriptorType::eSampledImage, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE}, {vk::DescriptorType::eSampler, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE}}};
		vk::DescriptorPoolCreateInfo pool_info{
			vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
			0,
			pool_sizes
		};
		for (VkDescriptorPoolSize &pool_size : pool_sizes) {
			pool_info.maxSets += pool_size.descriptorCount;
		}

		PFN_vkCreateDescriptorPool func;
		{
			std::shared_lock lock(global_lock);
			func = device_dispatch[get_key(device)].CreateDescriptorPool;
		}

		VkDescriptorPool pool;
		auto result = func(
			device,
			reinterpret_cast<const VkDescriptorPoolCreateInfo *>(&pool_info),
			nullptr,
			&pool
		);
		if (result != VK_SUCCESS) {
			l.error("Cannot create vk::DescriptorPool");
		}
		descriptor_pool = pool;
		update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
			info.DescriptorPool = pool;
		});
		l.info("Created vk::DescriptorPool");
	}
	void create_render_pass(VkDevice device, VkFormat format) {
		VkAttachmentDescription attachment{};
		attachment.format = format;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // don't clear, we're overlaying
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference color_ref{
			.attachment = 0,
			.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
		};

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color_ref;

		VkRenderPassCreateInfo rp_info{};
		rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		rp_info.attachmentCount = 1;
		rp_info.pAttachments = &attachment;
		rp_info.subpassCount = 1;
		rp_info.pSubpasses = &subpass;

		VkRenderPass rpass;
		PFN_vkCreateRenderPass func;
		{
			std::shared_lock lock(global_lock);
			func = device_dispatch[get_key(device)].CreateRenderPass;
		}

		auto result = func(device, &rp_info, nullptr, &rpass); // imgui's loaded fn
		l.info("Created RenderPass: {}", result == VK_SUCCESS);

		update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
			info.PipelineInfoMain.RenderPass = rpass;
			info.PipelineInfoMain.Subpass = 0;
		});

		render_pass = rpass;
	}
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
	void init_swapchain_data(VkSwapchainKHR sw, std::span<VkImage> data, VkFormat image_format, uint32_t height, uint32_t width) {
		_swapchain_data.emplace(
			sw,
			swapchain_data{
				.width = width,
				.height = height,
				.images = std::vector<vk::Image>(data.begin(), data.end()),
				.image_format = vk::Format(image_format),
			}
		);
	}
	void remove_swapchain_data(VkSwapchainKHR sw) {
		_swapchain_data.erase(sw);
	}
	private:

	std::map<vk::SwapchainKHR, swapchain_data> _swapchain_data;
};
