// =============================================================================
//  JsfxEmbedded.h  -  JSFX files compiled into the plugin binary
//
//  At build time cmake/scripts/embed_files.cmake turns the plugin folder
//  (main .jsfx, imports, data files, presets...) into a C++ source. At run time
//  the bundle is unpacked ONCE into a per-user cache folder, named after the
//  content hash, and ysfx loads it from there like any JSFX on disk. Identical
//  on Windows and macOS, no bundle-resource plumbing, and a new build never
//  reuses stale files.
// =============================================================================
#pragma once

#include <cstddef>
#include <string>

namespace jsfx {

struct EmbeddedFile
{
  const char* path;               // relative, '/' separated
  const unsigned char* data;
  size_t size;
};

struct EmbeddedBundle
{
  const EmbeddedFile* files;
  size_t count;
  const char* mainFile;           // relative path of the .jsfx to load
  const char* hash;               // content hash (cache folder name)
};

/** Per-user cache root: %LOCALAPPDATA%, ~/Library/Caches or $XDG_CACHE_HOME. UTF-8. */
std::string UserCacheDir();

/** Unpacks `bundle` into <UserCacheDir>/<vendor>/<product>/<hash> unless it is
 *  already there, and returns that folder (UTF-8, '/' separators). */
bool ExtractBundle(const EmbeddedBundle& bundle, const std::string& vendor, const std::string& product,
                   std::string& outDir, std::string& error);

} // namespace jsfx
