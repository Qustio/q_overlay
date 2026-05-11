add_executable(q_overlay_test "q_overlay_test.cpp")

target_link_libraries(q_overlay_test PRIVATE GTest::gtest_main q_overlay)

gtest_discover_tests(q_overlay_test)

if(ENABLE_COVERAGE)
  find_program(LCOV lcov)

  if(LCOV)
    add_custom_target(coverage
		COMMAND ${CMAKE_COMMAND} -E env LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/coverage-%p.profraw
			ctest --test-dir ${CMAKE_BINARY_DIR}
		COMMAND llvm-profdata merge -sparse ${CMAKE_BINARY_DIR}/coverage-*.profraw
				-o ${CMAKE_BINARY_DIR}/coverage.profdata
		COMMAND llvm-cov export $<TARGET_FILE:q_overlay_test>
				-instr-profile=${CMAKE_BINARY_DIR}/coverage.profdata
				-format=lcov
				-ignore-filename-regex='_deps'
				> ${CMAKE_SOURCE_DIR}/coverage.info
		COMMENT "Generating Clang source-based coverage report"
    )
  endif()
endif()