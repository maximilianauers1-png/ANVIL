cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ANVIL_VERSION_RAW)
string(STRIP "${ANVIL_VERSION_RAW}" ANVIL_VERSION)
project(ANVIL VERSION ${ANVIL_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ANVIL_BUILD_APP   "Build the Qt Quick desktop application" ON)
option(ANVIL_BUILD_TESTS "Build unit and acceptance tests" ON)
option(ANVIL_BUILD_GEOM  "Build the OpenCASCADE worker (anvil-geom)" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# All output binaries next to each other so the app finds python/ and qml/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/Versions.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Warnings.cmake)

add_subdirectory(core)
add_subdirectory(python)
if(ANVIL_BUILD_GEOM)
  add_subdirectory(workers/geom)
endif()
if(ANVIL_BUILD_APP)
  add_subdirectory(app)
endif()
if(ANVIL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Only the "anvil" component is packaged (cmake --install ... --component anvil):
# third-party projects built via FetchContent bring their own install rules
# (headers, import libraries) that must not end up in dist\ANVIL.
install(DIRECTORY python/anvil DESTINATION python COMPONENT anvil
        PATTERN "__pycache__" EXCLUDE)
install(DIRECTORY examples DESTINATION . COMPONENT anvil)
