# anvil-geom: OpenCASCADE in a separate process (ADR 0001). OCCT is linked
# dynamically here and nowhere else.
find_package(OpenCASCADE CONFIG QUIET)
if(NOT OpenCASCADE_FOUND)
  message(STATUS "OpenCASCADE not found: building ${ANVIL_OCCT_TAG} from source (first build takes a while)")
  set(BUILD_MODULE_ApplicationFramework OFF CACHE BOOL "" FORCE)
  set(BUILD_MODULE_Draw OFF CACHE BOOL "" FORCE)
  set(BUILD_MODULE_Visualization OFF CACHE BOOL "" FORCE)
  set(BUILD_MODULE_DETools OFF CACHE BOOL "" FORCE)
  set(BUILD_MODULE_DataExchange ON CACHE BOOL "" FORCE)
  set(BUILD_MODULE_ModelingAlgorithms ON CACHE BOOL "" FORCE)
  set(BUILD_MODULE_ModelingData ON CACHE BOOL "" FORCE)
  set(BUILD_MODULE_FoundationClasses ON CACHE BOOL "" FORCE)
  set(BUILD_DOC_Overview OFF CACHE BOOL "" FORCE)
  set(USE_FREETYPE OFF CACHE BOOL "" FORCE)
  set(USE_TK OFF CACHE BOOL "" FORCE)
  set(USE_TCL OFF CACHE BOOL "" FORCE)
  set(USE_OPENGL OFF CACHE BOOL "" FORCE)
  set(USE_GLES2 OFF CACHE BOOL "" FORCE)
  set(USE_D3D OFF CACHE BOOL "" FORCE)
  set(USE_XLIB OFF CACHE BOOL "" FORCE)
  set(USE_RAPIDJSON OFF CACHE BOOL "" FORCE)
  set(USE_DRACO OFF CACHE BOOL "" FORCE)
  set(USE_TBB OFF CACHE BOOL "" FORCE)
  set(USE_VTK OFF CACHE BOOL "" FORCE)
  set(BUILD_LIBRARY_TYPE "Shared" CACHE STRING "" FORCE)
  set(INSTALL_DIR_BIN "." CACHE STRING "" FORCE)
  # OCCT's own install rules: development files go to a folder the Windows
  # packaging deletes; the runtime DLLs are installed below via anvil-geom.
  foreach(_d INCLUDE LIB CMAKE RESOURCE DATA SAMPLES TESTS SCRIPT DOC)
    set(INSTALL_DIR_${_d} "occt-dev/${_d}" CACHE STRING "" FORCE)
  endforeach()
  FetchContent_Declare(occt
    GIT_REPOSITORY https://github.com/Open-Cascade-SAS/OCCT.git
    GIT_TAG ${ANVIL_OCCT_TAG}
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(occt)
endif()

set(_occt_toolkits TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool
    TKFillet TKOffset TKMesh TKShHealing TKXSBase TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209 TKDESTEP TKDE)
set(_occt_libs)
foreach(tk ${_occt_toolkits})
  if(TARGET ${tk})
    list(APPEND _occt_libs ${tk})
  endif()
endforeach()

add_executable(anvil-geom main.cpp)
target_link_libraries(anvil-geom PRIVATE ${_occt_libs} nlohmann_json::nlohmann_json)
if(OpenCASCADE_INCLUDE_DIR)
  target_include_directories(anvil-geom SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
endif()
if(MSVC)
  target_compile_options(anvil-geom PRIVATE /utf-8 /bigobj)
  target_compile_definitions(anvil-geom PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
endif()
install(TARGETS anvil-geom RUNTIME DESTINATION .)
if(WIN32)
  # Windows finds DLLs next to the executable: copy the OCCT toolkits there in
  # the build tree (tests start the worker from there) and install them.
  add_custom_command(TARGET anvil-geom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:anvil-geom> $<TARGET_FILE_DIR:anvil-geom>
    COMMAND_EXPAND_LISTS)
  install(FILES $<TARGET_RUNTIME_DLLS:anvil-geom> DESTINATION .)
endif()
