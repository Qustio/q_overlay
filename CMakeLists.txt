cmake_minimum_required(VERSION 3.28)

project(q_overlay VERSION 0.1 LANGUAGES CXX)

option(ENABLE_COVERAGE "Enable code coverage reporting" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(IMGUI_DIR third_party/imgui)

find_package(GTest REQUIRED)
find_package(spdlog REQUIRED)
find_package(Vulkan REQUIRED)
find_package(VulkanUtilityLibraries QUIET)

include(GoogleTest)

enable_testing()

add_library(q_overlay SHARED)

target_sources(q_overlay PRIVATE
	src/layer.cpp
	${IMGUI_DIR}/imgui.cpp
	${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
	${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

target_sources(q_overlay
	PRIVATE
	FILE_SET CXX_MODULES FILES
	src/global_state.cppm
)
target_link_libraries(q_overlay PRIVATE
	spdlog::spdlog
	Vulkan::Headers
)
if (VulkanUtilityLibraries_FOUND)
	target_link_libraries(q_overlay PRIVATE
		Vulkan::UtilityHeaders
		Vulkan::LayerSettings
	)
endif()
target_include_directories(q_overlay PRIVATE
	${IMGUI_DIR}
	${IMGUI_DIR}/backends
)
target_compile_definitions(q_overlay PRIVATE
	IMGUI_IMPL_VULKAN_NO_PROTOTYPES
)


set(LAYER_LIBRARY_PATH ${CMAKE_INSTALL_PREFIX}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}q_overlay${CMAKE_SHARED_LIBRARY_SUFFIX})
message(STATUS ${LAYER_LIBRARY_PATH})

if (WIN32)
	target_compile_definitions(q_overlay PUBLIC
		VK_USE_PLATFORM_WIN32_KHR
		WIN32_LEAN_AND_MEAN
		NOMINMAX
		NOGDI
	)
endif()

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_q_overlay.json.in
	${CMAKE_CURRENT_BINARY_DIR}/VkLayer_q_overlay.json
	@ONLY
)

if(ENABLE_COVERAGE)
	target_compile_options(q_overlay PUBLIC -fprofile-instr-generate -fcoverage-mapping)
	target_link_options(q_overlay PUBLIC -fprofile-instr-generate -fcoverage-mapping)
endif()

install(TARGETS q_overlay DESTINATION lib)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_q_overlay.json DESTINATION ${CMAKE_INSTALL_PREFIX}/share/vulkan/implicit_layer.d)

#add_subdirectory(test)
