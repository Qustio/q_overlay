cmake_minimum_required(VERSION 3.14)

project(q_overlay VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(IMGUI_DIR third_party/imgui)


find_package(spdlog REQUIRED)
find_package(Vulkan REQUIRED)
find_package(VulkanUtilityLibraries REQUIRED)

add_library(q_overlay SHARED
	layer.cpp
	${IMGUI_DIR}/imgui.cpp
	${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
	${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_link_libraries(q_overlay PRIVATE
	spdlog::spdlog
	Vulkan::Headers
	Vulkan::UtilityHeaders
	Vulkan::LayerSettings
)
target_include_directories(q_overlay PRIVATE
	${IMGUI_DIR}
	${IMGUI_DIR}/backends
)
target_compile_definitions(q_overlay PRIVATE
	IMGUI_IMPL_VULKAN_NO_PROTOTYPES
)
add_custom_command(TARGET q_overlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_q_overlay.json
        ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_q_overlay.json
)

install(TARGETS q_overlay DESTINATION lib)
install(FILES q_overlay.json DESTINATION share/vulkan/implicit_layer.d)