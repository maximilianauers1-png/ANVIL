# anvil-ai: loopback-only client of the local Ollama server (ADR 0001, 0010).
# The only ANVIL binary with sockets; no third-party networking library.
add_executable(anvil-ai main.cpp)
target_link_libraries(anvil-ai PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(anvil-ai PRIVATE cxx_std_20)
if(WIN32)
  target_link_libraries(anvil-ai PRIVATE ws2_32)
endif()
if(MSVC)
  target_compile_options(anvil-ai PRIVATE /utf-8)
  target_compile_definitions(anvil-ai PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
install(TARGETS anvil-ai RUNTIME DESTINATION . COMPONENT anvil)
