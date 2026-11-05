add_executable(anvil_core_tests unit/test_core.cpp)
target_link_libraries(anvil_core_tests PRIVATE anvil_core Catch2::Catch2WithMain)
anvil_warnings(anvil_core_tests)
include(Catch)
catch_discover_tests(anvil_core_tests)

# Python reference oracle (M0) stays part of the suite.
add_test(NAME reference_python
         COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${PROJECT_SOURCE_DIR}/tests/reference)

# M1 acceptance (script vs. UI command sequence, recovery, API suite).
add_test(NAME acceptance_m1
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/acceptance/test_m1_acceptance.py
                 $<TARGET_FILE:anvil-cli>)
