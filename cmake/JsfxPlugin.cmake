# =============================================================================
#  JsfxPlugin.cmake
#
#  jsfx_generate_sources(<plugin_dir> <main.jsfx> <out_dir> <prefix>)
#     build-time generation shared by plugins and tests. Creates
#       <prefix>_meta      custom target -> <out_dir>/jsfx_meta.h, jsfx_params.inc
#       <prefix>_embedded  object library compiled from <out_dir>/jsfx_embedded.cpp
#     Each generated file belongs to exactly one target, so several formats
#     (VST3, AU) can consume them in parallel builds (Xcode, MSBuild).
#  jsfx_use_generated(<target> <prefix> <out_dir>)
#
#  jsfx_add_plugin(<plugin_dir>)
#     reads <plugin_dir>/plugin.json and creates <Name>-vst3 (+ <Name>-au)
# =============================================================================
include_guard(GLOBAL)

set(JSFX_TEMPLATES_DIR "${CMAKE_CURRENT_LIST_DIR}/templates")
set(JSFX_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/scripts/embed_files.cmake")

function(jsfx_generate_sources plugin_dir main out_dir prefix)
  file(MAKE_DIRECTORY "${out_dir}")
  # Every file of the plugin folder is embedded; re-glob when files are added.
  file(GLOB_RECURSE _inputs CONFIGURE_DEPENDS "${plugin_dir}/*")

  add_custom_command(
    OUTPUT "${out_dir}/jsfx_meta.h" "${out_dir}/jsfx_params.inc"
    COMMAND jsfx_meta "${plugin_dir}/${main}" "${plugin_dir}" "${plugin_dir}/data" "${out_dir}"
    DEPENDS jsfx_meta ${_inputs}
    COMMENT "jsfx_meta: parameters and I/O of ${main}"
    VERBATIM)
  add_custom_target(${prefix}_meta DEPENDS "${out_dir}/jsfx_meta.h" "${out_dir}/jsfx_params.inc")

  add_custom_command(
    OUTPUT "${out_dir}/jsfx_embedded.cpp"
    COMMAND "${CMAKE_COMMAND}"
            "-DSRC_DIR=${plugin_dir}"
            "-DOUT=${out_dir}/jsfx_embedded.cpp"
            "-DMAIN=${main}"
            "-DEXCLUDE=plugin.json"
            -P "${JSFX_EMBED_SCRIPT}"
    DEPENDS "${JSFX_EMBED_SCRIPT}" ${_inputs}
    COMMENT "Embedding ${plugin_dir}"
    VERBATIM)
  add_library(${prefix}_embedded OBJECT "${out_dir}/jsfx_embedded.cpp")
  target_include_directories(${prefix}_embedded PRIVATE "${JSFX_FRAMEWORK_DIR}/engine")
  set_target_properties(${prefix}_embedded PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

function(jsfx_use_generated target prefix out_dir)
  add_dependencies(${target} ${prefix}_meta)
  target_sources(${target} PRIVATE $<TARGET_OBJECTS:${prefix}_embedded>)
  target_include_directories(${target} PRIVATE "${out_dir}")
endfunction()

# --- small helpers -----------------------------------------------------------
function(_jsfx_json_get out json key default)
  string(JSON _v ERROR_VARIABLE _err GET "${json}" "${key}")
  if(_err)
    set(_v "${default}")
  endif()
  set(${out} "${_v}" PARENT_SCOPE)
endfunction()

function(_jsfx_c_escape out str)
  string(REPLACE "\\" "\\\\" str "${str}")
  string(REPLACE "\"" "\\\"" str "${str}")
  set(${out} "${str}" PARENT_SCOPE)
endfunction()

function(_jsfx_xml_escape out str)
  string(REPLACE "&" "&amp;" str "${str}")
  string(REPLACE "<" "&lt;" str "${str}")
  string(REPLACE ">" "&gt;" str "${str}")
  set(${out} "${str}" PARENT_SCOPE)
endfunction()

function(_jsfx_bool out value)
  if(value)
    set(${out} 1 PARENT_SCOPE)
  else()
    set(${out} 0 PARENT_SCOPE)
  endif()
endfunction()

# -----------------------------------------------------------------------------
function(jsfx_add_plugin plugin_dir)
  file(READ "${plugin_dir}/plugin.json" _json)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${plugin_dir}/plugin.json")

  _jsfx_json_get(JSFX_NAME "${_json}" name "")
  if(NOT JSFX_NAME MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
    message(FATAL_ERROR "${plugin_dir}/plugin.json: \"name\" must be a C identifier (letters, digits, _), got '${JSFX_NAME}'")
  endif()
  _jsfx_json_get(JSFX_MAIN "${_json}" jsfx "${JSFX_NAME}.jsfx")
  if(NOT EXISTS "${plugin_dir}/${JSFX_MAIN}")
    message(FATAL_ERROR "${plugin_dir}: JSFX file '${JSFX_MAIN}' not found")
  endif()

  _jsfx_json_get(_type "${_json}" type "effect")
  if(_type STREQUAL "instrument")
    set(JSFX_PLUG_TYPE 1)
    set(JSFX_AU_TYPE aumu)
    set(_midi_in_default ON)
    set(_vst3_default "Instrument|Synth")
    set(JSFX_CLAP_FEATURE "instrument")
  elseif(_type STREQUAL "midi-effect")
    set(JSFX_PLUG_TYPE 2)
    set(JSFX_AU_TYPE aumi)
    set(_midi_in_default ON)
    set(_vst3_default "Fx")
    set(JSFX_CLAP_FEATURE "note-effect")
  elseif(_type STREQUAL "effect")
    set(JSFX_PLUG_TYPE 0)
    set(_midi_in_default OFF)
    set(_vst3_default "Fx")
    set(JSFX_CLAP_FEATURE "audio-effect")
  else()
    message(FATAL_ERROR "${plugin_dir}/plugin.json: \"type\" must be instrument, effect or midi-effect")
  endif()

  _jsfx_json_get(_midi_in "${_json}" midi_in ${_midi_in_default})
  _jsfx_json_get(_midi_out "${_json}" midi_out OFF)
  if(_type STREQUAL "midi-effect")
    set(_midi_out ON)
  endif()
  _jsfx_bool(JSFX_MIDI_IN ${_midi_in})
  _jsfx_bool(JSFX_MIDI_OUT ${_midi_out})
  if(_type STREQUAL "effect")
    if(JSFX_MIDI_IN)
      set(JSFX_AU_TYPE aumf)   # effect that receives MIDI
    else()
      set(JSFX_AU_TYPE aufx)
    endif()
  endif()

  _jsfx_json_get(JSFX_VERSION "${_json}" version "1.0.0")
  if(NOT JSFX_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "${plugin_dir}/plugin.json: \"version\" must look like 1.2.3")
  endif()
  # iPlug2 / AU layout: 0xMMMMmmpp
  math(EXPR JSFX_VERSION_INT "(${CMAKE_MATCH_1} << 16) | (${CMAKE_MATCH_2} << 8) | ${CMAKE_MATCH_3}")
  math(EXPR JSFX_VERSION_HEX "${JSFX_VERSION_INT}" OUTPUT_FORMAT HEXADECIMAL)
  string(REGEX REPLACE "^0x" "" _hex "${JSFX_VERSION_HEX}")
  string(LENGTH "${_hex}" _len)
  while(_len LESS 8)
    set(_hex "0${_hex}")
    string(LENGTH "${_hex}" _len)
  endwhile()
  set(JSFX_VERSION_HEX "0x${_hex}")

  _jsfx_json_get(_mfr "${_json}" manufacturer "")
  if(_mfr STREQUAL "")
    message(FATAL_ERROR "${plugin_dir}/plugin.json: \"manufacturer\" is required")
  endif()
  _jsfx_c_escape(JSFX_MFR "${_mfr}")
  string(REGEX REPLACE "[^A-Za-z0-9-]" "" JSFX_BUNDLE_MFR "${_mfr}")   # for bundle ids

  _jsfx_json_get(JSFX_MFR_CODE "${_json}" manufacturer_code "")
  _jsfx_json_get(JSFX_PLUGIN_CODE "${_json}" plugin_code "")
  foreach(_code JSFX_MFR_CODE JSFX_PLUGIN_CODE)
    string(LENGTH "${${_code}}" _l)
    if(NOT _l EQUAL 4 OR NOT "${${_code}}" MATCHES "^[A-Za-z0-9]+$")
      message(FATAL_ERROR "${plugin_dir}/plugin.json: manufacturer_code and plugin_code must be 4 letters/digits")
    endif()
  endforeach()
  if(JSFX_MFR_CODE MATCHES "^[a-z0-9]+$")
    message(WARNING "${JSFX_NAME}: Apple reserves all-lowercase manufacturer codes; use at least one uppercase letter")
  endif()

  _jsfx_json_get(JSFX_BUNDLE_DOMAIN "${_json}" bundle_domain "com")
  _jsfx_json_get(_sub "${_json}" vst3_subcategory "${_vst3_default}")
  _jsfx_c_escape(JSFX_VST3_SUBCATEGORY "${_sub}")
  _jsfx_json_get(_url "${_json}" url "")
  _jsfx_c_escape(JSFX_URL "${_url}")
  _jsfx_json_get(_email "${_json}" email "")
  _jsfx_c_escape(JSFX_EMAIL "${_email}")
  _jsfx_json_get(_copy "${_json}" copyright "")
  _jsfx_c_escape(JSFX_COPYRIGHT "${_copy}")
  _jsfx_xml_escape(JSFX_COPYRIGHT_XML "${_copy}")
  _jsfx_xml_escape(JSFX_MFR_XML "${_mfr}")
  _jsfx_json_get(JSFX_UI_WIDTH "${_json}" ui_width 0)
  _jsfx_json_get(JSFX_UI_HEIGHT "${_json}" ui_height 0)

  set(JSFX_MIN_MACOS "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(NOT JSFX_MIN_MACOS)
    set(JSFX_MIN_MACOS "10.15")
  endif()

  if(JSFX_DEV_RELOAD)
    set(JSFX_DEV_RELOAD_FLAG 1)
    set(JSFX_DEV_SOURCE_DIR "${plugin_dir}")
  else()
    set(JSFX_DEV_RELOAD_FLAG 0)
    set(JSFX_DEV_SOURCE_DIR "")
  endif()

  # --------------------------------------------------------------- generation
  set(_gen "${CMAKE_BINARY_DIR}/plugins/${JSFX_NAME}")
  jsfx_generate_sources("${plugin_dir}" "${JSFX_MAIN}" "${_gen}" _${JSFX_NAME})
  configure_file("${JSFX_TEMPLATES_DIR}/config.h.in" "${_gen}/config.h" @ONLY)
  # iplug_configure_target() looks for <Name>-<FORMAT>-Info.plist in PLUG_RESOURCES_DIR
  set(PLUG_RESOURCES_DIR "${_gen}/resources")
  configure_file("${JSFX_TEMPLATES_DIR}/VST3-Info.plist.in" "${PLUG_RESOURCES_DIR}/${JSFX_NAME}-VST3-Info.plist" @ONLY)
  configure_file("${JSFX_TEMPLATES_DIR}/AU-Info.plist.in" "${PLUG_RESOURCES_DIR}/${JSFX_NAME}-AU-Info.plist" @ONLY)

  set(_sources
    "${JSFX_FRAMEWORK_DIR}/plugin/JsfxPlugin.cpp"
    "${JSFX_FRAMEWORK_DIR}/plugin/JsfxPlugin.h"
    "${JSFX_FRAMEWORK_DIR}/plugin/JsfxGfxControl.cpp"
    "${JSFX_FRAMEWORK_DIR}/plugin/JsfxGfxControl.h"
    "${_gen}/config.h")

  # shared settings of all formats of this plugin
  add_library(_${JSFX_NAME}-base INTERFACE)
  target_include_directories(_${JSFX_NAME}-base INTERFACE "${_gen}" "${JSFX_FRAMEWORK_DIR}/plugin")
  target_link_libraries(_${JSFX_NAME}-base INTERFACE iPlug2::IPlug jsfx::engine)

  set(_targets "")
  if("VST3" IN_LIST JSFX_PLUGIN_FORMATS)
    add_library(${JSFX_NAME}-vst3 MODULE ${_sources})
    iplug_add_target(${JSFX_NAME}-vst3 PUBLIC LINK iPlug2::VST3 ${IGRAPHICS_LIB} _${JSFX_NAME}-base)
    iplug_configure_target(${JSFX_NAME}-vst3 VST3 ${JSFX_NAME})
    jsfx_link_runtime(${JSFX_NAME}-vst3 SWELL_${JSFX_NAME}_vst3_)
    list(APPEND _targets ${JSFX_NAME}-vst3)
  endif()
  if(APPLE AND "AU" IN_LIST JSFX_PLUGIN_FORMATS)
    add_library(${JSFX_NAME}-au MODULE ${_sources})
    iplug_add_target(${JSFX_NAME}-au PUBLIC LINK iPlug2::AUv2 ${IGRAPHICS_LIB} _${JSFX_NAME}-base)
    iplug_configure_target(${JSFX_NAME}-au AUv2 ${JSFX_NAME})
    jsfx_link_runtime(${JSFX_NAME}-au SWELL_${JSFX_NAME}_au_)
    list(APPEND _targets ${JSFX_NAME}-au)
  endif()

  foreach(_t ${_targets})
    jsfx_use_generated(${_t} _${JSFX_NAME} "${_gen}")
    target_sources(${_t} PRIVATE $<TARGET_OBJECTS:jsfx_plugin_font>)
    jsfx_assert_single_wdl_target(${_t})
  endforeach()

  add_custom_target(${JSFX_NAME} DEPENDS ${_targets})
  message(STATUS "JSFX plugin ${JSFX_NAME}: ${_type}, ${JSFX_MAIN} -> ${_targets}")
endfunction()

# -----------------------------------------------------------------------------
# Walks everything a plugin target compiles (own sources + INTERFACE_SOURCES of
# every linked interface target) and fails if a WDL unit appears twice, or if
# one of jsfx_core's WDL units is compiled again by iPlug2.
# -----------------------------------------------------------------------------
function(_jsfx_collect_interface_sources target out_var visited_var)
  set(_visited ${${visited_var}})
  set(_result ${${out_var}})
  if(NOT TARGET ${target} OR target IN_LIST _visited)
    return()
  endif()
  list(APPEND _visited ${target})
  get_target_property(_type ${target} TYPE)
  get_target_property(_isrc ${target} INTERFACE_SOURCES)
  if(_isrc)
    list(APPEND _result ${_isrc})
  endif()
  get_target_property(_ilibs ${target} INTERFACE_LINK_LIBRARIES)
  if(_ilibs)
    foreach(_l ${_ilibs})
      if(TARGET ${_l})
        _jsfx_collect_interface_sources(${_l} _result _visited)
      endif()
    endforeach()
  endif()
  set(${out_var} ${_result} PARENT_SCOPE)
  set(${visited_var} ${_visited} PARENT_SCOPE)
endfunction()

function(jsfx_assert_single_wdl_target target)
  get_target_property(_own ${target} SOURCES)
  set(_all ${_own})
  set(_visited "")
  get_target_property(_libs ${target} LINK_LIBRARIES)
  foreach(_l ${_libs})
    _jsfx_collect_interface_sources(${_l} _all _visited)
  endforeach()

  set(_seen "")
  foreach(_s ${_all})
    if(NOT _s MATCHES "/WDL/[^$]*\\.(c|cpp|mm)$")
      continue()
    endif()
    string(REGEX REPLACE ".*/WDL/" "WDL/" _key "${_s}")
    if(_key IN_LIST _seen)
      message(FATAL_ERROR "Duplicate WDL unit in ${target}: ${_key} is compiled twice")
    endif()
    list(APPEND _seen "${_key}")
    # jsfx_core (static lib, not visible here) owns fft, eel2 and lice;
    # SWELL is added per target by jsfx_link_runtime()
    if(_key MATCHES "^WDL/(fft\\.c|eel2/|lice/|swell/)")
      message(FATAL_ERROR "Duplicate WDL unit in ${target}: iPlug2 compiles ${_key}, "
                          "which the JSFX runtime already provides")
    endif()
  endforeach()
endfunction()
