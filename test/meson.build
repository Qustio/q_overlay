gtest_dep = dependency('gtest_main', required : true)

q_overlay_test = executable('q_overlay_test',
  sources : ['q_overlay_test.cpp'],
  link_with : q_overlay,
  include_directories : include_directories('..'),
  dependencies : [
    gtest_dep,
    spdlog_dep,
    vulkan_dep,
  ],
)

test('q_overlay_test', q_overlay_test,
  protocol : 'gtest',
)