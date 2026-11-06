qt_standard_project_setup()

qt_add_executable(anvil WIN32
  src/main.cpp
  src/app_controller.cpp src/app_controller.hpp
  src/viewport_item.cpp src/viewport_item.hpp src/viewport_draw.cpp src/viewport_sculpt.cpp src/viewport_cut.cpp src/sculpt_engine.cpp src/sculpt_engine.hpp
  src/scene_builder.cpp src/scene_builder.hpp
  src/settings.cpp src/settings.hpp
  src/selftest.cpp src/selftest.hpp
  src/vk/renderer.cpp src/vk/renderer.hpp)

set(ANVIL_QML_FILES
  qml/Main.qml qml/Theme.qml qml/AButton.qml qml/AField.qml qml/ALabel.qml
  qml/TopBar.qml qml/CommandBar.qml qml/RadialMenu.qml qml/SnapOverlay.qml
  qml/ViewCube.qml qml/AxisTriad.qml qml/PropertiesPanel.qml qml/BottomPanel.qml
  qml/StatusBar.qml qml/ExportDialog.qml qml/RecoveryDialog.qml qml/FileDialogs.qml
  qml/RadialEditor.qml qml/DrawOverlay.qml qml/SketchEditor.qml qml/SketchBar.qml qml/SculptPanel.qml qml/MiniPanel.qml qml/SliderField.qml
  qml/CutOverlay.qml qml/ToolBox.qml qml/ToolIcons.qml)
set_source_files_properties(qml/Theme.qml qml/ToolIcons.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
# Place the files at the module root (qrc:/qt/qml/Anvil/X.qml) so the implicit
# directory import is the module itself (needed for the Theme singleton).
foreach(f ${ANVIL_QML_FILES})
  get_filename_component(_name ${f} NAME)
  set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${_name})
endforeach()
if(Qt6_VERSION VERSION_LESS 6.6)
  # Qt < 6.6 qmlcachegen miscompiles some singleton lookups (hang in
  # AOTCompiledContext::loadSingletonLookup). Use the interpreter there; the
  # Windows target (Qt >= 6.8) keeps ahead-of-time compiled QML.
  set_source_files_properties(${ANVIL_QML_FILES} PROPERTIES QT_QML_SKIP_CACHEGEN TRUE)
endif()

# QML is compiled ahead of time (qmlcachegen) for a fast cold start (ADR 0002).
qt_add_qml_module(anvil
  URI Anvil
  VERSION 1.0
  RESOURCE_PREFIX /qt/qml
  NO_PLUGIN
  QML_FILES ${ANVIL_QML_FILES})

target_include_directories(anvil PRIVATE src)
target_compile_definitions(anvil PRIVATE ANVIL_APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(anvil PRIVATE anvil_session anvil_vulkan_headers
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
anvil_warnings(anvil)
if(WIN32)
  set_target_properties(anvil PROPERTIES OUTPUT_NAME "ANVIL")
endif()
install(TARGETS anvil RUNTIME DESTINATION . COMPONENT anvil BUNDLE DESTINATION . COMPONENT anvil)
