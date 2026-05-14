cmake_minimum_required(VERSION 3.14)

project(q_overlay VERSION 0.1 LANGUAGES CXX)

option(ENABLE_COVERAGE "Enable code coverage reporting" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(IMGUI_DIR third_party/imgui)

find_package(GTest REQUIRED)
find_package(spdlog REQUIRED)
find_package(Vulkan REQUIRED)
find_package(VulkanUtilityLibraries QUIET)

include(GoogleTest)

enable_testing()

add_library(q_overlay SHARED
	layer.cpp
	${IMGUI_DIR}/imgui.cpp
	${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
	${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(
	q_overlay PUBLIC
	headers
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
add_custom_command(TARGET q_overlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_q_overlay.json
        ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_q_overlay.json
)

if(ENABLE_COVERAGE)
	target_compile_options(q_overlay PUBLIC -fprofile-instr-generate -fcoverage-mapping)
	target_link_options(q_overlay PUBLIC -fprofile-instr-generate -fcoverage-mapping)
endif()

install(TARGETS q_overlay DESTINATION lib)
install(FILES VkLayer_q_overlay.json DESTINATION share/vulkan/implicit_layer.d)

add_subdirectory(test)
