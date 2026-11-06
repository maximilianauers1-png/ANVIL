add_library(anvil_core STATIC
  src/units.cpp src/expr.cpp src/mesh.cpp src/bvh.cpp src/validate.cpp src/stl.cpp
  src/fsutil.cpp src/geometry.cpp src/document.cpp src/journal.cpp src/camera.cpp
  src/snap.cpp src/radial.cpp src/fuzzy.cpp src/version.cpp src/sketch.cpp src/subprocess.cpp src/geom_client.cpp src/sdf.cpp src/figure.cpp src/winding.cpp src/model_io.cpp src/rig.cpp src/weapon.cpp src/cut.cpp)
target_include_directories(anvil_core PUBLIC include)
target_compile_features(anvil_core PUBLIC cxx_std_20)
target_compile_definitions(anvil_core PRIVATE ANVIL_VERSION_STRING="${PROJECT_VERSION}")
find_package(Threads REQUIRED)
target_link_libraries(anvil_core PUBLIC manifold::manifold Threads::Threads PRIVATE Eigen3::Eigen nlohmann_json::nlohmann_json Clipper2)
anvil_warnings(anvil_core)
