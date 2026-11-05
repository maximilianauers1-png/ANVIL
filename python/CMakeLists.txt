# Embedded Python session (command bus) + the `_anvil` bridge module.
add_library(anvil_session STATIC src/session.cpp src/bindings.cpp src/paths.cpp)
target_include_directories(anvil_session PUBLIC src)
target_link_libraries(anvil_session PUBLIC anvil_core pybind11::embed)
target_compile_definitions(anvil_session PRIVATE
  ANVIL_SOURCE_PYTHON_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
anvil_warnings(anvil_session)

add_executable(anvil-cli src/cli.cpp)
target_link_libraries(anvil-cli PRIVATE anvil_session)
anvil_warnings(anvil-cli)
install(TARGETS anvil-cli RUNTIME DESTINATION . COMPONENT anvil)
