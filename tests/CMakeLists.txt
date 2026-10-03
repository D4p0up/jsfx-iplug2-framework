# Engine tests: run a JSFX without any plugin host.
# They use their own copy of the example synth (tests/fixtures/TestSynth), so
# the folders in plugins/ can be renamed, replaced or deleted freely.
set(_plugin "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/TestSynth")
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
jsfx_generate_sources("${_plugin}" "TestSynth.jsfx" "${_gen}" _smoke)

add_executable(engine_smoke engine_smoke.cpp)
jsfx_use_generated(engine_smoke _smoke "${_gen}")
target_link_libraries(engine_smoke PRIVATE jsfx::engine)
jsfx_link_runtime(engine_smoke SWELL_jsfx_smoke_)
jsfx_link_win32_utf8(engine_smoke)
if(WIN32)
  target_compile_definitions(engine_smoke PRIVATE NOMINMAX)
endif()

add_test(NAME engine_smoke COMMAND engine_smoke "${CMAKE_CURRENT_BINARY_DIR}/cache")

# Concurrency stress (audio + @gfx + state recall at the same time)
add_executable(engine_stress engine_stress.cpp)
jsfx_use_generated(engine_stress _smoke "${_gen}")
target_link_libraries(engine_stress PRIVATE jsfx::engine)
jsfx_link_runtime(engine_stress SWELL_jsfx_stress_)
jsfx_link_win32_utf8(engine_stress)
add_test(NAME engine_stress COMMAND engine_stress "${CMAKE_CURRENT_BINARY_DIR}/cache")
