# Tests.
#
#   test_layout, test_widgets, test_xml, test_threads, test_memory, test_animation,
#   test_text_editor, test_shell, test_dialogs, test_themes, test_markdown, test_tools,
#   test_locale, test_xmlc, test_xmlcpp (an .xmlc and an .xmlc++ compiled by ravenc)
#       run headless (null platform + null backend): no display and no GPU are
#       needed, so they run on any build machine.
#   test_render
#       renders real frames with the Vulkan backend and checks pixels. It is
#       reported as skipped when the machine has no usable Vulkan driver.
#
#   raven_benchmark
#       not a test: prints layout and paint timings for a large widget tree.
#
#   ctest --output-on-failure

function(raven_add_test name)
	add_executable(${name} ${name}.c test.h)
	target_link_libraries(${name} PRIVATE raven::raven)
	set_target_properties(${name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON FOLDER "tests")
	if(NOT WIN32)
		target_link_libraries(${name} PRIVATE m)
	endif()
	if(MSVC)
		target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
	endif()
	if(NOT RAVEN_WITH_STB_IMAGE)
		target_compile_definitions(${name} PRIVATE RAVEN_TEST_NO_IMAGE_DECODER)
	endif()
	add_test(NAME ${name} COMMAND ${name})
endfunction()

raven_add_test(test_layout)
raven_add_test(test_widgets)
raven_add_test(test_xml)
raven_add_test(test_threads)
raven_add_test(test_memory)
raven_add_test(test_animation)
raven_add_test(test_text_editor)
raven_add_test(test_shell)
raven_add_test(test_dock_tearing)
raven_add_test(test_editor_features)
raven_add_test(test_dialogs)
raven_add_test(test_themes)
raven_add_test(test_markdown)
raven_add_test(test_tools)
raven_add_test(test_locale)
raven_add_test(test_host_platform)

# an interface in XMLC (XML + C), compiled by ravenc
add_executable(test_xmlc test_xmlc.xmlc test.h)
target_link_libraries(test_xmlc PRIVATE raven::raven)
target_include_directories(test_xmlc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(test_xmlc PROPERTIES C_STANDARD 11 FOLDER "tests")
if(NOT WIN32)
	target_link_libraries(test_xmlc PRIVATE m)
endif()
if(MSVC)
	target_compile_definitions(test_xmlc PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
raven_target_xmlc(test_xmlc)
add_test(NAME test_xmlc COMMAND test_xmlc)

# the same in C++ (.xmlc++)
enable_language(CXX)
add_executable(test_xmlcpp test_xmlcpp.xmlc++ test.h)
target_link_libraries(test_xmlcpp PRIVATE raven::raven)
target_include_directories(test_xmlcpp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(test_xmlcpp PROPERTIES CXX_STANDARD 17 FOLDER "tests")
if(MSVC)
	target_compile_definitions(test_xmlcpp PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
raven_target_xmlc(test_xmlcpp)
add_test(NAME test_xmlcpp COMMAND test_xmlcpp)

if(RAVEN_BACKEND_VULKAN)
	raven_add_test(test_render)
	set_tests_properties(test_render PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Not a test: prints timings for a large widget tree (see the top of benchmark.c).
add_executable(raven_benchmark benchmark.c)
target_link_libraries(raven_benchmark PRIVATE raven::raven)
set_target_properties(raven_benchmark PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON FOLDER "tests")
if(MSVC)
	target_compile_definitions(raven_benchmark PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
