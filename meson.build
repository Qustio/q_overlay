project('q_overlay', 'cpp',
  version : '0.1',
  default_options : [
    'cpp_std=c++17',
    'cpp_eh=default',
  ],
)

imgui_dir = 'third_party/imgui'

# Dependencies
spdlog_dep = dependency('spdlog', required : true)
vulkan_dep = dependency('Vulkan', required : true)
vulkan_utility_dep = dependency('VulkanUtilityLibraries',
  method : 'cmake',
  modules : ['Vulkan::UtilityHeaders', 'Vulkan::LayerSettings'],
  required : true,
)

# ImGui sources
imgui_sources = [
  imgui_dir / 'imgui.cpp',
  imgui_dir / 'imgui_draw.cpp',
  imgui_dir / 'imgui_tables.cpp',
  imgui_dir / 'imgui_widgets.cpp',
  imgui_dir / 'backends' / 'imgui_impl_vulkan.cpp',
]

# Shared library
q_overlay = shared_library('q_overlay',
  sources : ['layer.cpp'] + imgui_sources,
  include_directories : include_directories(
    imgui_dir,
    imgui_dir / 'backends',
  ),
  dependencies : [
    spdlog_dep,
    vulkan_dep,
    vulkan_utility_dep,
  ],
  cpp_args : [
    '-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES',
  ],
  install : true,
  install_dir : get_option('libdir'),
)

# Copy the layer manifest next to the build output
configure_file(
  input : 'VkLayer_q_overlay.json',
  output : 'VkLayer_q_overlay.json',
  copy : true,
)

# Install the layer manifest
install_data('VkLayer_q_overlay.json',
  install_dir : get_option('datadir') / 'vulkan' / 'implicit_layer.d',
)

# Tests
subdir('test')