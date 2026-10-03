// =============================================================================
//  JsfxGfxRunner.h  -  runs a JSFX @gfx section on its own thread
//
//  ysfx renders @gfx into a CPU framebuffer (LICE). This class owns a worker
//  thread that, on request, feeds input to the VM, runs @gfx and publishes the
//  resulting picture as RGBA bytes. The UI side (any toolkit) only:
//    - posts frame requests with the current input state     RequestFrame()
//    - uploads finished frames to a texture                  ConsumeFrame()
//    - shows popup menus that @gfx asks for (gfx_showmenu)    TakeMenuRequest()
//                                                             AnswerMenu()
//  Running @gfx off the UI thread is what ysfx's own plugin does; it is also
//  required because gfx_showmenu() is synchronous for the JSFX while popup
//  menus are asynchronous in iPlug2 on macOS.
// =============================================================================
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "JsfxEngine.h"

namespace jsfx {

struct GfxInput
{
  float mouseX = 0.f, mouseY = 0.f;   // logical (toolkit) coordinates
  uint32_t buttons = 0;               // ysfx_button_*
  uint32_t mods = 0;                  // ysfx_mod_*
  double wheel = 0.0, hwheel = 0.0;   // accumulated notches
  bool hasFocus = false, visible = true, mouseOver = false;
  struct Key { uint32_t mods, key; bool press; };
  std::vector<Key> keys;
};

struct GfxMenuRequest
{
  std::string spec;                   // gfx_showmenu() string
  float x = 0.f, y = 0.f;             // logical coordinates
};

class GfxRunner
{
public:
  explicit GfxRunner(Engine& engine);
  ~GfxRunner();
  GfxRunner(const GfxRunner&) = delete;
  GfxRunner& operator=(const GfxRunner&) = delete;

  // ----------------------------------------------------------------- UI thread
  /** True while a frame is being rendered: keep accumulating input meanwhile. */
  bool IsBusy() const { return mBusy.load(); }
  /** Renders one frame of size (logicalW x logicalH). backingScale is the
   *  screen pixel ratio; it is used only if the JSFX sets gfx_ext_retina. */
  void RequestFrame(int logicalW, int logicalH, double backingScale, GfxInput input);
  /** Frame rate the JSFX asked for (gfx_ext_flags / default 30). */
  uint32_t RequestedFps() const { return mFps.load(); }

  /** Serial number of the latest finished frame (0 = none yet). */
  uint64_t LatestFrame() const { return mLatestSerial.load(); }
  /** Calls f(rgba, pixelW, pixelH) if a frame newer than `serial` exists. */
  bool ConsumeFrame(uint64_t& serial, const std::function<void(const uint8_t*, int, int)>& f);

  bool TakeMenuRequest(GfxMenuRequest& req);
  void AnswerMenu(int itemId);        // 0 = dismissed

  /** Windows OCR_* cursor id requested by @gfx, or -1 if unchanged. */
  int TakeCursorRequest() { return mCursor.exchange(-1); }
  void SetDroppedFiles(std::vector<std::string> paths);

  /** Unblocks a pending gfx_showmenu() (returns 0 to the JSFX). Any thread. */
  void Interrupt();

private:
  void ThreadMain();
  void RunFrame();

  static int32_t ShowMenuCb(void* user, const char* spec, int32_t x, int32_t y);
  static void SetCursorCb(void* user, int32_t cursor);
  static const char* GetDropFileCb(void* user, int32_t index);

  Engine& mEngine;
  std::thread mThread;
  std::mutex mMutex;                  // request + menu handshake
  std::condition_variable mCv;
  bool mStop = false;
  bool mHasRequest = false;
  std::atomic<bool> mBusy{false};

  // current request (copied under mMutex)
  int mReqW = 0, mReqH = 0;
  double mReqScale = 1.0;
  GfxInput mReqInput;
  double mFrameScale = 1.0;           // scale used by the frame being rendered

  // back buffer (worker only) / front buffer (shared)
  std::vector<uint32_t> mBack;
  int mBackW = 0, mBackH = 0;
  std::mutex mFrontMutex;
  std::vector<uint8_t> mFront;
  int mFrontW = 0, mFrontH = 0;
  uint64_t mFrontSerial = 0;
  std::atomic<uint64_t> mLatestSerial{0};
  bool mForcePublish = true;

  // menu handshake
  enum class MenuState { None, Pending, Shown, Answered };
  MenuState mMenuState = MenuState::None;
  GfxMenuRequest mMenuReq;
  int mMenuResult = 0;
  bool mInterrupted = false;

  std::atomic<int> mCursor{-1};
  std::atomic<uint32_t> mFps{30};

  std::mutex mDropMutex;
  std::vector<std::string> mDropped;
  std::string mDropReturn;            // keeps the returned c_str() alive
};

} // namespace jsfx
