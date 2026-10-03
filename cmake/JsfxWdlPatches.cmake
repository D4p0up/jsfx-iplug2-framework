# =============================================================================
#  Configure-time patches applied to *copies* of two WDL files.
#
#  ysfx needs a locale-independent atof() inside EEL2 and LineParser (otherwise
#  "0.5" is mis-parsed when the host runs with a French/German LC_NUMERIC).
#  ysfx ships its own patched WDL copy for that; we don't use it, because we
#  compile everything against the single WDL that ships with iPlug2.
#
#  Instead of editing the iPlug2 submodule, we copy the two affected files into
#  the build tree, inject the same hooks ysfx uses (NSEEL_ATOF and
#  WDL_LINEPARSE_ATOF) and compile the copies. If upstream WDL ever gains the
#  hooks, the copy is used unchanged. If the code we patch disappears,
#  configuration fails loudly instead of silently shipping a locale bug.
# =============================================================================

function(jsfx_patch_wdl wdl_dir out_root)
  set(_out_wdl "${out_root}/WDL")
  file(MAKE_DIRECTORY "${_out_wdl}/eel2")

  # ---------------------------------------------------------------------------
  # eel2/nseel-compiler.c : number literals go through NSEEL_ATOF
  # ---------------------------------------------------------------------------
  set(_src "${wdl_dir}/eel2/nseel-compiler.c")
  file(READ "${_src}" _txt)
  if(NOT _txt MATCHES "NSEEL_ATOF")
    set(_needle "(EEL_F)atof(tmp)")
    string(FIND "${_txt}" "${_needle}" _pos)
    if(_pos EQUAL -1)
      message(FATAL_ERROR "jsfx_patch_wdl: '${_needle}' not found in ${_src}. "
                          "WDL changed upstream; update cmake/JsfxWdlPatches.cmake.")
    endif()
    string(REPLACE "${_needle}" "(EEL_F)NSEEL_ATOF(tmp)" _txt "${_txt}")
    set(_hook
"/* --- jsfx-iplug2-framework: locale independent number parsing (same hook as ysfx) --- */
#ifdef NSEEL_ATOF
  double NSEEL_ATOF(const char *);
#else
  #define NSEEL_ATOF atof
#endif
/* --- end hook --- */
")
    # insert right after the first include of ns-eel-int.h
    string(FIND "${_txt}" "#include \"ns-eel-int.h\"" _inc)
    if(_inc EQUAL -1)
      message(FATAL_ERROR "jsfx_patch_wdl: include anchor not found in ${_src}")
    endif()
    string(SUBSTRING "${_txt}" 0 ${_inc} _head)
    string(SUBSTRING "${_txt}" ${_inc} -1 _tail)
    string(FIND "${_tail}" "\n" _eol)
    math(EXPR _eol "${_eol} + 1")
    string(SUBSTRING "${_tail}" 0 ${_eol} _incline)
    string(SUBSTRING "${_tail}" ${_eol} -1 _rest)
    set(_txt "${_head}${_incline}${_hook}${_rest}")
  endif()
  # The copy lives in another directory: make its relative includes absolute
  # so "ns-eel-int.h", "../denormal.h", "glue_*.h"... still resolve to iPlug2's WDL.
  string(REGEX REPLACE "#include \"\\.\\./([^\"]+)\"" "#include \"${wdl_dir}/\\1\"" _txt "${_txt}")
  string(REGEX REPLACE "#include \"((ns-eel|glue_|asm-nseel)[^\"]*)\"" "#include \"${wdl_dir}/eel2/\\1\"" _txt "${_txt}")
  _jsfx_write_if_changed("${_out_wdl}/eel2/nseel-compiler.c" "${_txt}")

  # ---------------------------------------------------------------------------
  # lineparse.h : gettoken_float() goes through WDL_LINEPARSE_ATOF
  # ---------------------------------------------------------------------------
  set(_src "${wdl_dir}/lineparse.h")
  file(READ "${_src}" _txt)
  if(NOT _txt MATCHES "WDL_LINEPARSE_ATOF")
    set(_needle "return atof(buf);")
    string(FIND "${_txt}" "${_needle}" _pos)
    if(_pos EQUAL -1)
      message(FATAL_ERROR "jsfx_patch_wdl: '${_needle}' not found in ${_src}. "
                          "WDL changed upstream; update cmake/JsfxWdlPatches.cmake.")
    endif()
    string(REPLACE "${_needle}" "return WDL_LINEPARSE_ATOF(buf);" _txt "${_txt}")
    string(REPLACE "#include \"heapbuf.h\""
"#include \"heapbuf.h\"
/* --- jsfx-iplug2-framework: locale independent atof (same hook as ysfx) --- */
#ifdef WDL_LINEPARSE_ATOF
  extern \"C\" double WDL_LINEPARSE_ATOF(const char *);
#else
  #define WDL_LINEPARSE_ATOF atof
#endif
/* --- end hook --- */" _txt "${_txt}")
  endif()
  string(REGEX REPLACE "#include \"([a-z_]+\\.h)\"" "#include \"${wdl_dir}/\\1\"" _txt "${_txt}")
  _jsfx_write_if_changed("${_out_wdl}/lineparse.h" "${_txt}")
endfunction()

# Avoid touching the file (and triggering rebuilds) when nothing changed.
function(_jsfx_write_if_changed path content)
  if(EXISTS "${path}")
    file(READ "${path}" _old)
    if(_old STREQUAL content)
      return()
    endif()
  endif()
  file(WRITE "${path}" "${content}")
endfunction()
