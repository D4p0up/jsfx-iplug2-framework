#include "JsfxEmbedded.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>

#if defined(_WIN32)
  #include <windows.h>
  #include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace jsfx {

namespace {

fs::path FromUtf8(const std::string& s)
{
#if defined(_WIN32)
  return fs::u8path(s);
#else
  return fs::path(s);
#endif
}

std::string ToUtf8(const fs::path& p)
{
#if defined(__cpp_char8_t)
  const auto u8 = p.generic_u8string();
  return std::string(u8.begin(), u8.end());
#else
  return p.generic_u8string();
#endif
}

// Only plain relative paths: no "..", no absolute paths, no drive letters.
bool IsSafeRelative(const std::string& rel)
{
  if (rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find(':') != std::string::npos)
    return false;
  for (const auto& part : FromUtf8(rel))
    if (part == "..")
      return false;
  return true;
}

bool WriteAll(const fs::path& dir, const EmbeddedBundle& b, std::string& error)
{
  for (size_t i = 0; i < b.count; ++i)
  {
    const EmbeddedFile& f = b.files[i];
    if (!IsSafeRelative(f.path))
    {
      error = std::string("unsafe embedded path: ") + f.path;
      return false;
    }
    const fs::path target = dir / FromUtf8(f.path);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out)
    {
      error = "cannot write " + ToUtf8(target);
      return false;
    }
    if (f.size)
      out.write(reinterpret_cast<const char*>(f.data), static_cast<std::streamsize>(f.size));
    if (!out)
    {
      error = "write failed: " + ToUtf8(target);
      return false;
    }
  }
  std::ofstream(dir / ".complete") << "ok";
  return true;
}

} // namespace

std::string UserCacheDir()
{
  // explicit override (tests, portable setups)
  if (const char* env = std::getenv("JSFX_CACHE_DIR"); env && *env)
    return env;
#if defined(_WIN32)
  PWSTR wpath = nullptr;
  std::string result;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wpath)) && wpath)
    result = ToUtf8(fs::path(wpath));
  if (wpath) CoTaskMemFree(wpath);
  if (result.empty())
    result = ToUtf8(fs::temp_directory_path());
  return result;
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  return home ? std::string(home) + "/Library/Caches" : ToUtf8(fs::temp_directory_path());
#else
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
    return xdg;
  const char* home = std::getenv("HOME");
  return home ? std::string(home) + "/.cache" : ToUtf8(fs::temp_directory_path());
#endif
}

bool ExtractBundle(const EmbeddedBundle& bundle, const std::string& vendor, const std::string& product,
                   std::string& outDir, std::string& error)
{
  const fs::path base = FromUtf8(UserCacheDir()) / FromUtf8(vendor) / FromUtf8(product);
  const fs::path finalDir = base / FromUtf8(bundle.hash);
  std::error_code ec;

  if (fs::exists(finalDir / ".complete", ec))
  {
    outDir = ToUtf8(finalDir);
    return true;
  }

  // Unpack into a private temporary folder, then rename atomically: several
  // plugin instances (or hosts) may start at the same moment.
  std::mt19937_64 rng(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path tmpDir = base / (std::string(bundle.hash) + ".tmp" + std::to_string(rng() % 1000000007ull));
  fs::create_directories(tmpDir, ec);
  if (ec)
  {
    error = "cannot create " + ToUtf8(tmpDir) + ": " + ec.message();
    return false;
  }
  if (!WriteAll(tmpDir, bundle, error))
  {
    fs::remove_all(tmpDir, ec);
    return false;
  }

  fs::rename(tmpDir, finalDir, ec);
  if (ec)
  {
    // somebody else won the race, or a broken leftover is in the way
    fs::remove_all(tmpDir, ec);
    if (!fs::exists(finalDir / ".complete", ec))
    {
      fs::remove_all(finalDir, ec);
      if (!WriteAll(finalDir, bundle, error))
        return false;
    }
  }
  outDir = ToUtf8(finalDir);
  return true;
}

} // namespace jsfx
