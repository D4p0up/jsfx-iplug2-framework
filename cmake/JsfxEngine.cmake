# =============================================================================
#  JsfxEngine.cmake
#
#  Builds the JSFX runtime (ysfx core + EEL2 + LICE + WDL fft) against the ONE
#  WDL copy that ships inside iPlug2. ysfx's own vendored WDL
#  (third_party/ysfx/thirdparty/WDL) is never compiled nor put on an include
#  path, so every WDL class/function exists in exactly one version in the final
#  plugin binary.
#
#  Who compiles which WDL translation unit (plugin targets):
#
#    WDL file                    | iPlug2 (VST3/AU)       | this file
#    ----------------------------+------------------------+----------------------
#    win32_utf8.c   (Windows)    | yes (iPlug2::IPlug)    | NO for plugins
#                                |                        | (only standalone tools)
#    fft.c                       | no                     | yes  (jsfx_core)
#    eel2/*.c + asm object       | no                     | yes  (jsfx_core)
#    lice/*.cpp                  | no (NanoVG/Skia)       | yes  (jsfx_core)
#    swell/*  (macOS)            | no (APP format only)   | yes, per plugin target
#                                |                        | with a unique ObjC prefix
#    header-only (ptrlist.h...)  | shared, same version   | shared, same version
#
#  jsfx_assert_single_wdl() re-checks the first column at configure time, so an
#  iPlug2 update that starts compiling one of these files fails the configure
#  step instead of silently linking two copies.
#
#  Inputs : IPLUG2_DIR, JSFX_YSFX_DIR
#  Outputs: jsfx::core           static lib (ysfx + EEL2 + LICE + fft)
#           jsfx_link_runtime()  adds SWELL + EEL2 glue object + frameworks to a
#                                final binary (plugin, tool, test)
#           jsfx_link_win32_utf8() adds win32_utf8.c to a standalone exe (Windows)
# =============================================================================

include_guard(GLOBAL)
include(CheckCSourceCompiles)
include(${CMAKE_CURRENT_LIST_DIR}/JsfxWdlPatches.cmake)

if(NOT IPLUG2_DIR)
  message(FATAL_ERROR "JsfxEngine.cmake: IPLUG2_DIR must be set")
endif()
if(NOT JSFX_YSFX_DIR)
  set(JSFX_YSFX_DIR "${CMAKE_SOURCE_DIR}/third_party/ysfx")
endif()
if(NOT EXISTS "${JSFX_YSFX_DIR}/include/ysfx.h")
  message(FATAL_ERROR "ysfx not found in ${JSFX_YSFX_DIR}. Run: git submodule update --init --recursive")
endif()
if(NOT EXISTS "${JSFX_YSFX_DIR}/thirdparty/dr_libs/dr_wav.h")
  message(FATAL_ERROR "ysfx submodule dr_libs missing. Run: git submodule update --init --recursive")
endif()

set(JSFX_WDL_DIR "${IPLUG2_DIR}/WDL")          # the single WDL
set(JSFX_WDL_ROOT "${IPLUG2_DIR}")             # so that "WDL/xxx.h" resolves to it
set(JSFX_PATCHED_ROOT "${CMAKE_BINARY_DIR}/jsfx_wdl_patched")
jsfx_patch_wdl("${JSFX_WDL_DIR}" "${JSFX_PATCHED_ROOT}")

find_package(Threads REQUIRED)

# EEL2 normally JIT-compiles JSFX code to machine code. Native Logic Pro on
# Apple Silicon runs AUs in a sandbox that forbids executable memory, so JIT
# code would crash there. AUTO = portable (interpreted) EEL2 for the arm64
# slice of macOS builds only, JIT everywhere else. Chosen per architecture
# at compile time, so it also works inside universal binaries.
set(JSFX_EEL2_MODE "AUTO" CACHE STRING "EEL2 backend: AUTO, JIT or PORTABLE")
set_property(CACHE JSFX_EEL2_MODE PROPERTY STRINGS AUTO JIT PORTABLE)
option(JSFX_EEL2_USE_NASM "Assemble the EEL2 x64 glue with NASM instead of using WDL's prebuilt object" OFF)

# -----------------------------------------------------------------------------
# Common compile settings shared by every JSFX runtime translation unit.
# Kept in an INTERFACE target linked PRIVATE, so none of these defines leak to
# iPlug2 code (e.g. WDL_FFT_REALSIZE only matters to fft.c and eel_fft.h users).
# -----------------------------------------------------------------------------
add_library(jsfx_build_settings INTERFACE)
target_compile_definitions(jsfx_build_settings INTERFACE
  WDL_FFT_REALSIZE=8
  NSEEL_ATOF=ysfx_wdl_atof
  WDL_LINEPARSE_ATOF=ysfx_wdl_atof
  YSFX_API=                                 # static linking: no export attribute
  $<$<NOT:$<PLATFORM_ID:Windows>>:_FILE_OFFSET_BITS=64>
  $<$<PLATFORM_ID:Windows>:NOMINMAX>
  $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
  $<$<PLATFORM_ID:Windows>:_CRT_NONSTDC_NO_WARNINGS>
  $<$<PLATFORM_ID:Linux>:SWELL_LICE_GDI>
)
target_include_directories(jsfx_build_settings INTERFACE
  "${JSFX_PATCHED_ROOT}"                    # patched WDL/lineparse.h wins
  "${JSFX_WDL_ROOT}"                        # then iPlug2's WDL for "WDL/..."
)
if(NOT MSVC)
  # wdltypes.h wants signed char; ARM defaults to unsigned
  target_compile_options(jsfx_build_settings INTERFACE -fsigned-char -Wno-unused-parameter
    -Wno-multichar $<$<COMPILE_LANGUAGE:CXX>:-Wno-ignored-attributes>)
endif()
if(MSVC)
  target_compile_options(jsfx_build_settings INTERFACE /wd4244 /wd4267 /wd4305 /wd4996)
endif()

function(_jsfx_hidden target)
  set_target_properties(${target} PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    OBJC_VISIBILITY_PRESET hidden
    OBJCXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
endfunction()

# -----------------------------------------------------------------------------
# EEL2 machine-code glue. Its constants (NSEEL_RAM_BLOCKS...) must match
# eel2/ns-eel.h, so it must come from the SAME WDL. We use the objects WDL
# ships prebuilt (no NASM needed in CI); NASM is only needed on Linux x64.
# -----------------------------------------------------------------------------
set(JSFX_EEL2_ASM_OBJECT "")
if(JSFX_EEL2_MODE STREQUAL "PORTABLE")
  set(_eel2_portable ON)
else()
  set(_eel2_portable OFF)
endif()
if(NOT _eel2_portable)
  if(MSVC)
    if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64EC")
      set(JSFX_EEL2_ASM_OBJECT "${JSFX_WDL_DIR}/eel2/asm-nseel-arm64ec.obj")
    elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64")
      set(JSFX_EEL2_ASM_OBJECT "${JSFX_WDL_DIR}/eel2/asm-nseel-aarch64-msvc.obj")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
      if(JSFX_EEL2_USE_NASM)
        find_program(NASM_PROGRAM nasm REQUIRED)
        set(JSFX_EEL2_ASM_OBJECT "${CMAKE_BINARY_DIR}/jsfx_eel2/asm-nseel-x64.obj")
        add_custom_command(OUTPUT "${JSFX_EEL2_ASM_OBJECT}"
          COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/jsfx_eel2"
          COMMAND "${NASM_PROGRAM}" -f win64 -o "${JSFX_EEL2_ASM_OBJECT}" "${JSFX_WDL_DIR}/eel2/asm-nseel-x64-sse.asm"
          DEPENDS "${JSFX_WDL_DIR}/eel2/asm-nseel-x64-sse.asm" VERBATIM)
      else()
        set(JSFX_EEL2_ASM_OBJECT "${JSFX_WDL_DIR}/eel2/asm-nseel-x64.obj")
      endif()
    endif() # 32-bit x86 MSVC uses inline asm (asm-nseel-x86-msvc.c), no object
  elseif(APPLE)
    # universal object: full x86_64 slice + empty arm64 slice (arm64 glue is C)
    set(JSFX_EEL2_ASM_OBJECT "${JSFX_WDL_DIR}/eel2/asm-nseel-multi-macho.o")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    find_program(NASM_PROGRAM nasm)
    if(NASM_PROGRAM)
      set(JSFX_EEL2_ASM_OBJECT "${CMAKE_BINARY_DIR}/jsfx_eel2/asm-nseel-x64-sse.o")
      add_custom_command(OUTPUT "${JSFX_EEL2_ASM_OBJECT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/jsfx_eel2"
        COMMAND "${NASM_PROGRAM}" -D AMD64ABI -f elf64 -o "${JSFX_EEL2_ASM_OBJECT}" "${JSFX_WDL_DIR}/eel2/asm-nseel-x64-sse.asm"
        DEPENDS "${JSFX_WDL_DIR}/eel2/asm-nseel-x64-sse.asm" VERBATIM)
    else()
      message(WARNING "nasm not found: EEL2 falls back to the portable (slower) backend")
      set(_eel2_portable ON)
    endif()
  endif() # Linux aarch64: glue is C (asm-nseel-aarch64-gcc.c)
endif()

set(_eel2_asm_generated FALSE)
if(JSFX_EEL2_ASM_OBJECT)
  string(FIND "${JSFX_EEL2_ASM_OBJECT}" "${CMAKE_BINARY_DIR}/" _p)
  if(_p EQUAL 0)
    set(_eel2_asm_generated TRUE)
  elseif(NOT EXISTS "${JSFX_EEL2_ASM_OBJECT}")
    message(FATAL_ERROR "EEL2 glue object missing from iPlug2's WDL: ${JSFX_EEL2_ASM_OBJECT}")
  endif()
endif()

# One header, force-included in every EEL2/ysfx translation unit, decides the
# backend. EEL_TARGET_PORTABLE changes EEL2 data structures: it must be the
# same in all of them.
if(_eel2_portable)
  set(_mode_line "#define JSFX_EEL2_PORTABLE_ALWAYS 1")
elseif(JSFX_EEL2_MODE STREQUAL "AUTO")
  set(_mode_line "#define JSFX_EEL2_PORTABLE_ON_APPLE_ARM 1")
else()
  set(_mode_line "/* JIT everywhere */")
endif()
_jsfx_write_if_changed("${JSFX_PATCHED_ROOT}/jsfx_eel2_config.h" "/* generated by cmake/JsfxEngine.cmake (JSFX_EEL2_MODE=${JSFX_EEL2_MODE}) */
#pragma once
${_mode_line}
#if defined(JSFX_EEL2_PORTABLE_ALWAYS) || (defined(JSFX_EEL2_PORTABLE_ON_APPLE_ARM) && defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)))
  #ifndef EEL_TARGET_PORTABLE
    #define EEL_TARGET_PORTABLE
  #endif
#endif
")
if(MSVC)
  target_compile_options(jsfx_build_settings INTERFACE "/FI${JSFX_PATCHED_ROOT}/jsfx_eel2_config.h")
else()
  target_compile_options(jsfx_build_settings INTERFACE "SHELL:-include \"${JSFX_PATCHED_ROOT}/jsfx_eel2_config.h\"")
endif()

# -----------------------------------------------------------------------------
# WDL translation units that iPlug2 does NOT compile for plugins
# -----------------------------------------------------------------------------
set(_eel2 "${JSFX_WDL_DIR}/eel2")
set(_lice "${JSFX_WDL_DIR}/lice")
set(_jsfx_wdl_sources
  "${JSFX_WDL_DIR}/fft.c"
  # EEL2 (compiler comes from the patched copy, see JsfxWdlPatches.cmake)
  "${JSFX_PATCHED_ROOT}/WDL/eel2/nseel-compiler.c"
  "${_eel2}/nseel-caltab.c"
  "${_eel2}/nseel-cfunc.c"
  "${_eel2}/nseel-eval.c"
  "${_eel2}/nseel-lextab.c"
  "${_eel2}/nseel-ram.c"
  "${_eel2}/nseel-yylex.c"
  # LICE (software rasterizer used by JSFX @gfx)
  "${_lice}/lice.cpp"
  "${_lice}/lice_arc.cpp"
  "${_lice}/lice_colorspace.cpp"
  "${_lice}/lice_image.cpp"
  "${_lice}/lice_line.cpp"
  "${_lice}/lice_palette.cpp"
  "${_lice}/lice_texgen.cpp"
  "${_lice}/lice_text.cpp"
  "${_lice}/lice_textnew.cpp"
  # ysfx's stb-based image loaders for gfx_loadimg (stb symbols are static)
  "${JSFX_YSFX_DIR}/sources/lice_stb/lice_stb_loaders.cpp"
  "${JSFX_YSFX_DIR}/sources/lice_stb/lice_stb_bmp.cpp"
  "${JSFX_YSFX_DIR}/sources/lice_stb/lice_stb_gif.cpp"
  "${JSFX_YSFX_DIR}/sources/lice_stb/lice_stb_jpg.cpp"
  "${JSFX_YSFX_DIR}/sources/lice_stb/lice_stb_png.cpp"
  "${JSFX_YSFX_DIR}/sources/lice_stb/lice_stb_write.cpp"
)

# -----------------------------------------------------------------------------
# ysfx core sources (same list as ysfx's cmake.ysfx.txt)
# -----------------------------------------------------------------------------
set(_ys "${JSFX_YSFX_DIR}/sources")
set(_jsfx_ysfx_sources
  "${_ys}/ysfx.cpp"
  "${_ys}/ysfx_config.cpp"
  "${_ys}/ysfx_midi.cpp"
  "${_ys}/ysfx_reader.cpp"
  "${_ys}/ysfx_parse.cpp"
  "${_ys}/ysfx_parse_menu.cpp"
  "${_ys}/ysfx_preset.cpp"
  "${_ys}/ysfx_audio_wav.cpp"
  "${_ys}/ysfx_audio_flac.cpp"
  "${_ys}/ysfx_utils.cpp"
  "${_ys}/ysfx_utils_fts.cpp"
  "${_ys}/ysfx_api_eel.cpp"
  "${_ys}/ysfx_gmem.cpp"
  "${_ys}/ysfx_api_reaper.cpp"
  "${_ys}/ysfx_api_file.cpp"
  "${_ys}/ysfx_api_gfx.cpp"
  "${_ys}/ysfx_eel_utils.cpp"
  "${_ys}/ysfx_preprocess.cpp"
  # framework helper that needs ysfx internals (ysfx.hpp)
  "${CMAKE_CURRENT_LIST_DIR}/../framework/engine/JsfxYsfxBridge.cpp"
)
if(WIN32)
  set(_jsfx_has_fts FALSE)
else()
  check_c_source_compiles("#include <fts.h>\nint main(){fts_close((FTS*)0);return 0;}" JSFX_HAVE_FTS)
  set(_jsfx_has_fts ${JSFX_HAVE_FTS})
endif()

# -----------------------------------------------------------------------------
# jsfx::core : one static library compiled directly from both source lists.
# (Not from $<TARGET_OBJECTS> of object libraries: the Xcode generator
#  produces no .a at all for a library that has no source file of its own.)
# -----------------------------------------------------------------------------
add_library(jsfx_core STATIC ${_jsfx_wdl_sources} ${_jsfx_ysfx_sources})
target_link_libraries(jsfx_core PRIVATE jsfx_build_settings)
target_include_directories(jsfx_core PRIVATE
  "${_ys}"
  "${JSFX_YSFX_DIR}/thirdparty/dr_libs"
  "${JSFX_YSFX_DIR}/thirdparty/stb"
)
if(NOT _jsfx_has_fts)
  set_source_files_properties(${_jsfx_ysfx_sources} TARGET_DIRECTORY jsfx_core
                              PROPERTIES COMPILE_DEFINITIONS YSFX_NO_FTS)
endif()
set_target_properties(jsfx_core PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(jsfx_core PUBLIC "${JSFX_YSFX_DIR}/include")
target_compile_definitions(jsfx_core PUBLIC YSFX_API=)
target_link_libraries(jsfx_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(_eel2_asm_generated)
  add_custom_target(jsfx_eel2_asm DEPENDS "${JSFX_EEL2_ASM_OBJECT}")
endif()
# NOTE: the EEL2 glue object and the Apple frameworks are deliberately NOT
# attached to jsfx_core (nor to any static library): with the Xcode generator,
# link items of a static library are handed to `libtool`, which rejects WDL's
# universal (fat) object file. They are added to final binaries only, by
# jsfx_link_runtime() below; the linker (ld64) accepts fat objects.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(jsfx_core INTERFACE "LINKER:-z,noexecstack")
endif()
if(WIN32)
  target_link_libraries(jsfx_core PUBLIC gdi32 user32 shlwapi msimg32)
endif()
_jsfx_hidden(jsfx_core)
add_library(jsfx::core ALIAS jsfx_core)

# -----------------------------------------------------------------------------
# SWELL (macOS/Linux): LICE text rendering uses the Win32 GDI API, which SWELL
# provides off Windows. Added to each FINAL binary (not to jsfx_core) because
# its Objective-C classes must carry a prefix unique to that binary: two
# bundles registering the same ObjC class name crash at runtime (flat
# ObjC namespace). iPlug2 itself only compiles SWELL for the APP format.
# -----------------------------------------------------------------------------
function(_jsfx_link_swell target prefix)
  set(_sw "${JSFX_WDL_DIR}/swell")
  if(APPLE)
    set(_mm
      "${_sw}/swell-appstub.mm" "${_sw}/swell-dlg.mm" "${_sw}/swell-gdi.mm"
      "${_sw}/swell-kb.mm" "${_sw}/swell-menu.mm" "${_sw}/swell-misc.mm"
      "${_sw}/swell-miscdlg.mm" "${_sw}/swell-modstub.mm" "${_sw}/swell-wnd.mm")
    set(_cpp "${_sw}/swell-ini.cpp" "${_sw}/swell.cpp")
    set(_lib ${target}_swell)
    add_library(${_lib} OBJECT ${_mm} ${_cpp})
    set_source_files_properties(${_mm} TARGET_DIRECTORY ${_lib} PROPERTIES LANGUAGE OBJCXX)
    target_compile_definitions(${_lib} PRIVATE SWELL_APP_PREFIX=${prefix})
    target_compile_options(${_lib} PRIVATE -Wno-deprecated-declarations -fno-objc-arc)
    target_link_libraries(${_lib} PRIVATE jsfx_build_settings)
    _jsfx_hidden(${_lib})
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:${_lib}>)
    target_link_libraries(${target} PRIVATE "-framework Cocoa" "-framework Carbon" "-framework Foundation" "-framework Metal")
  elseif(UNIX)
    set(_lib ${target}_swell)
    add_library(${_lib} OBJECT
      "${_sw}/swell-ini.cpp" "${_sw}/swell.cpp"
      "${_sw}/swell-appstub-generic.cpp" "${_sw}/swell-dlg-generic.cpp"
      "${_sw}/swell-gdi-generic.cpp" "${_sw}/swell-gdi-lice.cpp"
      "${_sw}/swell-generic-headless.cpp" "${_sw}/swell-kb-generic.cpp"
      "${_sw}/swell-menu-generic.cpp" "${_sw}/swell-misc-generic.cpp"
      "${_sw}/swell-miscdlg-generic.cpp" "${_sw}/swell-modstub-generic.cpp"
      "${_sw}/swell-wnd-generic.cpp")
    target_link_libraries(${_lib} PRIVATE jsfx_build_settings)
    find_package(Freetype QUIET)
    find_package(Fontconfig QUIET)
    if(TARGET Freetype::Freetype)
      target_compile_definitions(${_lib} PRIVATE SWELL_FREETYPE)
      target_link_libraries(${_lib} PRIVATE Freetype::Freetype)
      target_link_libraries(${target} PRIVATE Freetype::Freetype)
    endif()
    if(TARGET Fontconfig::Fontconfig)
      target_compile_definitions(${_lib} PRIVATE SWELL_FONTCONFIG)
      target_link_libraries(${_lib} PRIVATE Fontconfig::Fontconfig)
      target_link_libraries(${target} PRIVATE Fontconfig::Fontconfig)
    endif()
    _jsfx_hidden(${_lib})
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:${_lib}>)
  endif()
endfunction()

# -----------------------------------------------------------------------------
# jsfx_link_runtime(<target> <objc_prefix>)
#   Call once on every FINAL binary (plugin, tool, test) that uses jsfx::core:
#   SWELL with a per-binary ObjC prefix, the EEL2 glue object, the frameworks.
# -----------------------------------------------------------------------------
function(jsfx_link_runtime target prefix)
  _jsfx_link_swell(${target} ${prefix})
  if(JSFX_EEL2_ASM_OBJECT)
    # Passed as a raw linker argument, NOT via target_link_libraries(): the
    # Xcode generator puts a full-path .o link item both in OTHER_LDFLAGS and
    # in the "Link Binary With Libraries" phase, so ld sees it twice
    # (75 "duplicate symbol" errors). A link option is emitted exactly once
    # by every generator (Xcode, Visual Studio, Ninja, Makefiles).
    target_link_options(${target} PRIVATE "${JSFX_EEL2_ASM_OBJECT}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${JSFX_EEL2_ASM_OBJECT}")
    if(TARGET jsfx_eel2_asm)
      add_dependencies(${target} jsfx_eel2_asm)
    endif()
  endif()
  if(APPLE)
    target_link_libraries(${target} PRIVATE "-framework Cocoa" "-framework Carbon" "-framework Foundation" "-framework Metal")
  endif()
endfunction()

# win32_utf8.c is compiled by iPlug2 into every plugin target. Standalone
# executables that don't link iPlug2 (generator tool, tests) need it too.
# NEVER call this on an iPlug2 plugin target.
function(jsfx_link_win32_utf8 target)
  if(WIN32)
    target_sources(${target} PRIVATE "${JSFX_WDL_DIR}/win32_utf8.c")
    target_link_libraries(${target} PRIVATE jsfx_build_settings)
  endif()
endfunction()

# -----------------------------------------------------------------------------
# Guard: fail if iPlug2 starts compiling a WDL unit that jsfx_core also contains
# -----------------------------------------------------------------------------
function(jsfx_assert_single_wdl)
  set(_ours "/WDL/fft.c$" "/WDL/eel2/" "/WDL/lice/" "/WDL/swell/")
  foreach(_t ${ARGN})
    if(NOT TARGET ${_t})
      continue()
    endif()
    get_target_property(_srcs ${_t} INTERFACE_SOURCES)
    if(NOT _srcs)
      continue()
    endif()
    foreach(_s ${_srcs})
      foreach(_pat ${_ours})
        if(_s MATCHES "${_pat}")
          message(FATAL_ERROR "Duplicate WDL: iPlug2 target ${_t} compiles ${_s}, "
                              "which jsfx_core already contains.")
        endif()
      endforeach()
    endforeach()
  endforeach()
endfunction()
