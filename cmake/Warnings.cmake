function(anvil_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj
                                             /wd4251 /wd4275)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow
                                             -Wno-missing-field-initializers)
  endif()
endfunction()
