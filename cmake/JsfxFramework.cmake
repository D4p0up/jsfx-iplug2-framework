# =============================================================================
#  JsfxFramework.cmake
#    jsfx::engine     host-agnostic wrapper (framework/engine)
#    jsfx_meta        build-time generator (tools/jsfx_meta.cpp)
#    jsfx_add_plugin  one VST3 (+ AU on macOS) per plugins/<Name>/ folder
# =============================================================================
include_guard(GLOBAL)

set(JSFX_FRAMEWORK_DIR "${CMAKE_SOURCE_DIR}/framework")
set(JSFX_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# -----------------------------------------------------------------------------
# jsfx::engine
# -----------------------------------------------------------------------------
add_library(jsfx_engine STATIC
  "${JSFX_FRAMEWORK_DIR}/engine/JsfxEngine.cpp"
  "${JSFX_FRAMEWORK_DIR}/engine/JsfxEngine.h"
  "${JSFX_FRAMEWORK_DIR}/engine/JsfxGfxRunner.cpp"
  "${JSFX_FRAMEWORK_DIR}/engine/JsfxGfxRunner.h"
  "${JSFX_FRAMEWORK_DIR}/engine/JsfxEmbedded.cpp"
  "${JSFX_FRAMEWORK_DIR}/engine/JsfxEmbedded.h"
)
target_include_directories(jsfx_engine PUBLIC "${JSFX_FRAMEWORK_DIR}/engine")
target_link_libraries(jsfx_engine PUBLIC jsfx::core)
target_compile_features(jsfx_engine PUBLIC cxx_std_17)
if(WIN32)
  target_compile_definitions(jsfx_engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(jsfx_engine PUBLIC ole32 shell32 uuid)
endif()
set_target_properties(jsfx_engine PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON POSITION_INDEPENDENT_CODE ON)
add_library(jsfx::engine ALIAS jsfx_engine)

# -----------------------------------------------------------------------------
# jsfx_meta : runs on the build machine while building each plugin
# -----------------------------------------------------------------------------
add_executable(jsfx_meta "${CMAKE_SOURCE_DIR}/tools/jsfx_meta.cpp")
target_link_libraries(jsfx_meta PRIVATE jsfx::core)
jsfx_link_swell(jsfx_meta SWELL_jsfx_meta)
jsfx_link_win32_utf8(jsfx_meta)
if(APPLE)
  # a host tool: build it for the build machine only, not universal
  set_target_properties(jsfx_meta PROPERTIES OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()

include(${JSFX_CMAKE_DIR}/JsfxPlugin.cmake)

# Roboto, embedded once for the generic (no @gfx) UI and the error overlay
set(_font_dir "${JSFX_FRAMEWORK_DIR}/plugin/fonts")
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/jsfx_font/jsfx_font.cpp"
  COMMAND "${CMAKE_COMMAND}" "-DSRC_DIR=${_font_dir}" "-DOUT=${CMAKE_BINARY_DIR}/jsfx_font/jsfx_font.cpp"
          "-DMAIN=Roboto-Regular.ttf" "-DSYMBOL=kFont" -P "${JSFX_EMBED_SCRIPT}"
  DEPENDS "${_font_dir}/Roboto-Regular.ttf" "${JSFX_EMBED_SCRIPT}"
  COMMENT "Embedding Roboto-Regular.ttf"
  VERBATIM)
add_library(jsfx_plugin_font OBJECT "${CMAKE_BINARY_DIR}/jsfx_font/jsfx_font.cpp")
target_include_directories(jsfx_plugin_font PRIVATE "${JSFX_FRAMEWORK_DIR}/engine")
set_target_properties(jsfx_plugin_font PROPERTIES POSITION_INDEPENDENT_CODE ON)
