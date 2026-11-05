# anvil-geom: OpenCASCADE in a separate process (ADR 0001). OCCT is linked
# dynamically here and nowhere else.
find_package(OpenCASCADE CONFIG QUIET)
if(NOT OpenCASCADE_FOUND)
  message(FATAL_ERROR
    "OpenCASCADE ${ANVIL_OCCT_TAG} not found. Windows: run scripts\\build-occt.ps1 (builds it once into "
    ".deps\\occt) and configure with -DOpenCASCADE_DIR=<repo>/.deps/occt/cmake (build-windows.ps1 does both). "
    "Linux: install the distribution package (libocct-*-dev). Or configure with -DANVIL_BUILD_GEOM=OFF.")
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
install(TARGETS anvil-geom RUNTIME DESTINATION . COMPONENT anvil)
if(WIN32)
  # Windows finds DLLs next to the executable: copy the OCCT toolkits there in
  # the build tree (tests start the worker from there) and install them.
  add_custom_command(TARGET anvil-geom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:anvil-geom> $<TARGET_FILE_DIR:anvil-geom>
    COMMAND_EXPAND_LISTS)
  install(FILES $<TARGET_RUNTIME_DLLS:anvil-geom> DESTINATION . COMPONENT anvil)
endif()
