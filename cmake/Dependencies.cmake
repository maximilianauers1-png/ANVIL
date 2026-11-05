# Dependency resolution: find_package first (vcpkg or system), otherwise
# FetchContent from pinned git tags. Network is only used at build time.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- Manifold (mesh booleans) ------------------------------------------------
find_package(manifold 3.5 CONFIG QUIET)
if(NOT manifold_FOUND)
  set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
  set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
  set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)
  set(MANIFOLD_JSBIND OFF CACHE BOOL "" FORCE)
  # Serial backend: bit-identical results independent of thread count (ADR 0003).
  set(MANIFOLD_PAR OFF CACHE BOOL "" FORCE)
  set(MANIFOLD_CROSS_SECTION ON CACHE BOOL "" FORCE)
  set(MANIFOLD_USE_BUILTIN_CLIPPER2 ON CACHE BOOL "" FORCE)
  set(MANIFOLD_EXPORT OFF CACHE BOOL "" FORCE)
  set(MANIFOLD_DOWNLOADS ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_Declare(manifold
    GIT_REPOSITORY https://github.com/elalish/manifold.git
    GIT_TAG ${ANVIL_MANIFOLD_TAG}
    GIT_SHALLOW TRUE
    SYSTEM)
  FetchContent_MakeAvailable(manifold)
endif()

# --- Eigen (dense linear algebra for the sketch solver) ------------------------
find_package(Eigen3 3.4 CONFIG QUIET)
if(NOT TARGET Eigen3::Eigen)
  FetchContent_Declare(Eigen3
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG ${ANVIL_EIGEN_TAG}
    GIT_SHALLOW TRUE
    SYSTEM)
  set(EIGEN_BUILD_DOC OFF CACHE BOOL "" FORCE)
  set(EIGEN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF)
  FetchContent_GetProperties(Eigen3)
  if(NOT eigen3_POPULATED)
    FetchContent_Populate(Eigen3)
  endif()
  add_library(Eigen3::Eigen INTERFACE IMPORTED GLOBAL)
  target_include_directories(Eigen3::Eigen SYSTEM INTERFACE ${eigen3_SOURCE_DIR})
endif()

# --- Python (embedded) + pybind11 --------------------------------------------
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter Development.Embed)
find_package(pybind11 3.0 CONFIG QUIET)
if(NOT pybind11_FOUND)
  set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
  FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG ${ANVIL_PYBIND11_TAG}
    GIT_SHALLOW TRUE
    SYSTEM)
  FetchContent_MakeAvailable(pybind11)
endif()

# --- Catch2 (tests) -----------------------------------------------------------
if(ANVIL_BUILD_TESTS)
  find_package(Catch2 3 CONFIG QUIET)
  if(NOT Catch2_FOUND)
    FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG ${ANVIL_CATCH2_TAG}
      GIT_SHALLOW TRUE
      SYSTEM)
    FetchContent_MakeAvailable(Catch2)
  endif()
endif()

# --- Vulkan headers + Qt (app only) ------------------------------------------
# Headers only: the Vulkan loader ships with the GPU driver (vulkan-1.dll), so
# no Vulkan SDK is needed. Resolved before Qt so Qt's Vulkan support finds them.
if(ANVIL_BUILD_APP)
  find_package(Vulkan QUIET)
  add_library(anvil_vulkan_headers INTERFACE)
  if(Vulkan_FOUND)
    target_include_directories(anvil_vulkan_headers SYSTEM INTERFACE ${Vulkan_INCLUDE_DIRS})
  else()
    FetchContent_Declare(VulkanHeaders
      GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
      GIT_TAG ${ANVIL_VKHEADERS_TAG}
      GIT_SHALLOW TRUE
      SYSTEM)
    FetchContent_MakeAvailable(VulkanHeaders)
    set(Vulkan_INCLUDE_DIR "${vulkanheaders_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    target_include_directories(anvil_vulkan_headers SYSTEM INTERFACE "${vulkanheaders_SOURCE_DIR}/include")
  endif()
  find_package(Qt6 ${ANVIL_QT_MIN} REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
endif()
