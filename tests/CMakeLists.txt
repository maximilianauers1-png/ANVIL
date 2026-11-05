add_executable(anvil_core_tests unit/test_core.cpp unit/test_sketch.cpp)
target_link_libraries(anvil_core_tests PRIVATE anvil_core Catch2::Catch2WithMain)
anvil_warnings(anvil_core_tests)
include(Catch)
catch_discover_tests(anvil_core_tests)

# Python reference oracle (M0) stays part of the suite.
add_test(NAME reference_python
         COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${PROJECT_SOURCE_DIR}/tests/reference)
execute_process(COMMAND ${Python3_EXECUTABLE} -c "import numpy" RESULT_VARIABLE _np OUTPUT_QUIET ERROR_QUIET)
if(NOT _np EQUAL 0)
  message(WARNING "numpy not found for ${Python3_EXECUTABLE}: reference_python test disabled (pip install numpy)")
  set_tests_properties(reference_python PROPERTIES DISABLED TRUE)
endif()

# M1 acceptance (script vs. UI command sequence, recovery, API suite).
add_test(NAME acceptance_m1
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/acceptance/test_m1_acceptance.py
                 $<TARGET_FILE:anvil-cli>)

# End-to-end: the real app driven by synthetic input (needs a display; Xvfb on Linux).
if(ANVIL_BUILD_APP)
  add_test(NAME acceptance_m1_gui
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/acceptance/run_gui_selftest.py
                   $<TARGET_FILE:anvil> $<TARGET_FILE:anvil-cli> ${PROJECT_SOURCE_DIR}/examples/magnet_base.py)
  set_tests_properties(acceptance_m1_gui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400 LABELS gui)
endif()

add_test(NAME shaders_up_to_date COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/compile_shaders.py --check)
find_program(GLSLANG glslangValidator)
if(NOT GLSLANG)
  set_tests_properties(shaders_up_to_date PROPERTIES DISABLED TRUE)
endif()

# Core benchmarks (not a pass/fail test; run manually: anvil-bench --help)
add_executable(anvil-bench ${PROJECT_SOURCE_DIR}/bench/anvil_bench.cpp)
target_link_libraries(anvil-bench PRIVATE anvil_core)
anvil_warnings(anvil-bench)
install(TARGETS anvil-bench RUNTIME DESTINATION .)
