cmake_minimum_required(VERSION 3.25)

# Universal macOS binaries unless the caller chose otherwise (set before project())
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version (std::filesystem)")
endif()

project(JsfxPluginFramework VERSION 1.0.0 LANGUAGES C CXX)

set(IPLUG2_DIR "${CMAKE_SOURCE_DIR}/iPlug2" CACHE PATH "iPlug2 framework root (its WDL is the only WDL compiled)")
set(JSFX_YSFX_DIR "${CMAKE_SOURCE_DIR}/third_party/ysfx" CACHE PATH "ysfx root (only its core sources are used)")

if(WIN32 OR APPLE)
  set(_jsfx_plugins_default ON)
else()
  set(_jsfx_plugins_default OFF)   # iPlug2 has no Linux plugin support: engine + tests only
endif()
option(JSFX_BUILD_PLUGINS "Build the VST3/AU plugins found in plugins/" ${_jsfx_plugins_default})
option(JSFX_BUILD_TESTS "Build the engine smoke test" ON)
option(JSFX_DEV_RELOAD "Dev builds: load the .jsfx from the source tree and hot-reload it on save" OFF)
set(JSFX_PLUGIN_FORMATS "VST3;AU" CACHE STRING "Plugin formats to build (VST3, AU)")

if(JSFX_BUILD_PLUGINS)
  # Sets C++17, the static MSVC runtime and enables OBJC/OBJCXX on Apple.
  include(${IPLUG2_DIR}/iPlug2.cmake)
  find_package(iPlug2 REQUIRED)
else()
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  if(APPLE)
    enable_language(OBJC OBJCXX)
  endif()
endif()

include(cmake/JsfxEngine.cmake)      # jsfx::core  (ysfx + EEL2 + LICE on iPlug2's WDL)
include(cmake/JsfxFramework.cmake)   # jsfx::engine, jsfx_meta tool, jsfx_add_plugin()

if(JSFX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(JSFX_BUILD_PLUGINS)
  # Every folder in plugins/ that has a plugin.json becomes a plugin.
  file(GLOB _plugin_manifests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/plugins/*/plugin.json")
  foreach(_manifest ${_plugin_manifests})
    get_filename_component(_dir "${_manifest}" DIRECTORY)
    jsfx_add_plugin("${_dir}")
  endforeach()
endif()
