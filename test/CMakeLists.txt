add_executable(q_overlay_test "q_overlay_test.cpp")

target_link_libraries(q_overlay_test PRIVATE GTest::gtest_main q_overlay)

add_custom_command(
	TARGET q_overlay_test
	POST_BUILD
	COMMAND
		${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:q_overlay>
		$<TARGET_FILE_DIR:q_overlay_test>
)

gtest_discover_tests(q_overlay_test)

if(ENABLE_COVERAGE)
	find_program(LLVM-COV llvm-cov)
	find_program(LLVM-PROFDATA llvm-profdata)
	if(LLVM-COV AND LLVM-PROFDATA)
		set(PROFRAW_DIR $<TARGET_FILE_DIR:q_overlay_test>/profraw)
		add_custom_target(coverage
			COMMAND ${CMAKE_COMMAND} -E rm -rf ${PROFRAW_DIR}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${PROFRAW_DIR}
			COMMAND ${CMAKE_COMMAND} -E env LLVM_PROFILE_FILE=${PROFRAW_DIR}/coverage-%p.profraw
				ctest --test-dir ${CMAKE_BINARY_DIR}
			COMMAND ${LLVM-PROFDATA} merge
				-sparse ${PROFRAW_DIR}/coverage-*.profraw
				-o ${CMAKE_BINARY_DIR}/coverage.profdata
			COMMAND ${LLVM-COV} export $<TARGET_FILE:q_overlay_test>
				-object=$<TARGET_FILE:q_overlay>
				-instr-profile=${CMAKE_BINARY_DIR}/coverage.profdata
				-format=lcov
				-ignore-filename-regex=".*imgui.*|.*test.*"
				> ${CMAKE_SOURCE_DIR}/report.info
			COMMENT "Generating Clang source-based coverage report"
		)
	endif()
endif()
