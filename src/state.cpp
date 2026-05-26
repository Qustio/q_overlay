#include "misc/rcu.cpp"

#include <chrono>
#include <cstdint>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>
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

namespace {
	// use the loader's dispatch table pointer as a key for dispatch map lookups
	template <typename DispatchableType>
	auto get_key(const DispatchableType &inst) -> void * {
		if constexpr (requires { typename DispatchableType::NativeType; }) {
			auto ctype = static_cast<typename DispatchableType::NativeType>(inst);
			return *reinterpret_cast<void **>(ctype);
		} else {
			return *reinterpret_cast<void **>(inst);
		}
	}

	namespace state {
		using fn_map = std::map<void *, vk::detail::DispatchLoaderDynamic>;

		class state {
			struct swapchain_data {
				vk::Device device;
				vk::Extent2D extent;
				std::vector<vk::Image> images;
				std::vector<vk::UniqueImageView> image_views;
				vk::Format image_format;
				uint32_t image_count;

				std::vector<vk::UniqueCommandPool> cmd_pools;
				std::vector<vk::UniqueCommandBuffer> cmd_buffers;
				std::vector<vk::UniqueSemaphore> done_semaphores;
				std::vector<vk::UniqueFence> in_flight_fences;
			};
			using SwapchainMap = std::map<vk::SwapchainKHR, std::shared_ptr<const swapchain_data>>;
			public:

			state() : l(init_logger()) {
				ImGui::SetCurrentContext(imgui_ctx.get());
			}

			~state() {
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
#ifdef _WIN32
			struct {
				HWND hwnd = nullptr;
				WNDPROC original_wndproc = nullptr;
			} win32_hook;
#endif
			bool imgui_initialized{false};
			auto shutdown_imgui() -> void {
				if (imgui_initialized) {
					ImGui_ImplVulkan_Shutdown();
#ifdef _WIN32
					ImGui_ImplWin32_Shutdown();
#endif
					imgui_initialized = false;
					imgui_rendered.store(false);
				}
			}

			auto imgui() const -> ImDrawData * {
				if (!imgui_initialized) {
					return nullptr;
				}
				ImGui_ImplVulkan_NewFrame();
#ifdef _WIN32
				ImGui_ImplWin32_NewFrame();
#endif
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

				const auto &dld = device_dispatch.read()->at(get_key(device));
				swapchain_data entry{};
				entry.image_format = create_info.imageFormat;
				entry.extent = create_info.imageExtent;

				// images
				auto images = device.getSwapchainImagesKHR(sw, dld);
				if (!images) {
					l.error("Can't get swapchain images: {}", vk::to_string(images.error()));
					return;
				}
				entry.images = std::move(*images);
				entry.image_count = entry.images.size();
				update_imgui_init_info([&](ImGui_ImplVulkan_InitInfo &info) -> void {
					info.ImageCount = entry.image_count;
				});

				// reserve other vectors
				entry.image_views.reserve(entry.image_count);
				entry.cmd_pools.reserve(entry.image_count);
				entry.cmd_buffers.reserve(entry.image_count);
				entry.done_semaphores.reserve(entry.image_count);
				entry.in_flight_fences.reserve(entry.image_count);

				// todo use init_info_lock or rcu
				auto graphics_queue_index = init_info.QueueFamily;
				vk::ImageViewCreateInfo info{};
				info.viewType = vk::ImageViewType::e2D;
				info.format = create_info.imageFormat;
				info.subresourceRange = {
					vk::ImageAspectFlagBits::eColor,
					0,
					1, // mip
					0,
					1 // array
				};
				vk::CommandPoolCreateInfo pool_info{
					vk::CommandPoolCreateFlagBits::eTransient
						| vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
					graphics_queue_index
				};
				vk::CommandBufferAllocateInfo cmd_buf_info{
					{},
					vk::CommandBufferLevel::ePrimary,
					1
				};
				vk::FenceCreateInfo fence_info{vk::FenceCreateFlagBits::eSignaled};

				for (const auto &image : entry.images) {
					// image_views
					info.image = image;
					auto image_view = device.createImageViewUnique(info, nullptr, dld);
					if (!image_view) {
						l.error("Can't create image view: {}", vk::to_string(image_view.error()));
						return;
					}
					entry.image_views.push_back(std::move(*image_view));

					// cmd_pools
					auto pool = device.createCommandPoolUnique(pool_info, nullptr, dld);
					if (!pool) {
						l.error("Can't create command pool: {}", vk::to_string(pool.error()));
						return;
					}

					// cmd_buffers
					cmd_buf_info.commandPool = **pool;
					auto cmd_buffer = device.allocateCommandBuffersUnique(cmd_buf_info, dld);
					if (!cmd_buffer) {
						l.error("Can't allocate command buffer: {}", vk::to_string(cmd_buffer.error()));
						return;
					}

					// done_semaphores
					auto done_semaphore = device.createSemaphoreUnique({}, nullptr, dld);
					if (!done_semaphore) {
						l.error("Can't create semaphore: {}", vk::to_string(done_semaphore.error()));
						return;
					}

					// fences
					auto fence = device.createFenceUnique(fence_info, nullptr, dld);
					if (!fence) {
						l.error("Can't create fence: {}", vk::to_string(fence.error()));
						return;
					}

					entry.cmd_pools.push_back(std::move(*pool));
					entry.cmd_buffers.push_back(std::move((*cmd_buffer)[0]));
					entry.done_semaphores.push_back(std::move(*done_semaphore));
					entry.in_flight_fences.push_back(std::move(*fence));
				}

				_swapchain_data.mutate([&](SwapchainMap &sw_data) -> void {
					sw_data.emplace(
						sw, std::make_shared<const swapchain_data>(std::move(entry))
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
	} // namespace state
} // namespace
