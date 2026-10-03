#include "JsfxGfxRunner.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jsfx {

namespace {
// All instances of a plugin binary share EEL2/LICE globals, and ysfx does not
// support several @gfx running at once in one process (ysfx issue #44).
std::mutex& GlobalGfxRunMutex()
{
  static std::mutex m;
  return m;
}
} // namespace

GfxRunner::GfxRunner(Engine& engine)
: mEngine(engine)
{
  mEngine.SetGfxInterruptor([this] { Interrupt(); });
  mThread = std::thread([this] { ThreadMain(); });
}

GfxRunner::~GfxRunner()
{
  mEngine.SetGfxInterruptor(nullptr);
  {
    std::lock_guard<std::mutex> l(mMutex);
    mStop = true;
    mInterrupted = true;
  }
  mCv.notify_all();
  if (mThread.joinable())
    mThread.join();

  // The engine keeps pointing at our callbacks: detach them.
  std::lock_guard<std::mutex> g(mEngine.GfxMutex());
  if (ysfx_t* fx = mEngine.GfxFx())
  {
    ysfx_gfx_config_t gc{};
    gc.pixel_width = gc.pixel_height = 0;
    gc.scale_factor = 1.0;
    ysfx_gfx_setup(fx, &gc);
  }
}

// ----------------------------------------------------------------- UI thread
void GfxRunner::RequestFrame(int logicalW, int logicalH, double backingScale, GfxInput input)
{
  {
    std::lock_guard<std::mutex> l(mMutex);
    if (mHasRequest || mBusy.load()) return;
    mReqW = std::max(1, logicalW);
    mReqH = std::max(1, logicalH);
    mReqScale = backingScale > 0.0 ? backingScale : 1.0;
    mReqInput = std::move(input);
    mHasRequest = true;
    mBusy.store(true);
  }
  mCv.notify_all();
}

bool GfxRunner::ConsumeFrame(uint64_t& serial, const std::function<void(const uint8_t*, int, int)>& f)
{
  std::lock_guard<std::mutex> l(mFrontMutex);
  if (mFrontSerial == serial || mFront.empty())
    return false;
  serial = mFrontSerial;
  f(mFront.data(), mFrontW, mFrontH);
  return true;
}

bool GfxRunner::TakeMenuRequest(GfxMenuRequest& req)
{
  std::lock_guard<std::mutex> l(mMutex);
  if (mMenuState != MenuState::Pending)
    return false;
  req = mMenuReq;
  mMenuState = MenuState::Shown;
  return true;
}

void GfxRunner::AnswerMenu(int itemId)
{
  {
    std::lock_guard<std::mutex> l(mMutex);
    if (mMenuState != MenuState::Shown && mMenuState != MenuState::Pending)
      return;   // stale answer (menu was interrupted)
    mMenuResult = itemId;
    mMenuState = MenuState::Answered;
  }
  mCv.notify_all();
}

void GfxRunner::SetDroppedFiles(std::vector<std::string> paths)
{
  std::lock_guard<std::mutex> l(mDropMutex);
  mDropped = std::move(paths);
}

void GfxRunner::Interrupt()
{
  {
    std::lock_guard<std::mutex> l(mMutex);
    mInterrupted = true;
  }
  mCv.notify_all();
}

// --------------------------------------------------------------- worker side
void GfxRunner::ThreadMain()
{
  for (;;)
  {
    {
      std::unique_lock<std::mutex> l(mMutex);
      mCv.wait(l, [this] { return mStop || mHasRequest; });
      if (mStop) break;
      mHasRequest = false;
      mInterrupted = false;
    }
    RunFrame();
    mBusy.store(false);
  }
  mBusy.store(false);
}

void GfxRunner::RunFrame()
{
  int reqW, reqH;
  double reqScale;
  GfxInput in;
  {
    std::lock_guard<std::mutex> l(mMutex);
    reqW = mReqW; reqH = mReqH; reqScale = mReqScale;
    in = std::move(mReqInput);
  }

  std::lock_guard<std::mutex> gfxLock(mEngine.GfxMutex());
  ysfx_t* fx = mEngine.GfxFx();
  if (!fx || !ysfx_is_compiled(fx) || !ysfx_has_section(fx, ysfx_section_gfx))
    return;

  mFps.store(std::clamp<uint32_t>(ysfx_get_requested_framerate(fx), 1, 120));

  const double scale = ysfx_gfx_wants_retina(fx) ? reqScale : 1.0;
  mFrameScale = scale;
  // ysfx only writes gfx_ext_retina when the scale is > 1: after moving the
  // window from a Retina to a standard screen the JSFX would keep drawing at 2x.
  if (ysfx_gfx_wants_retina(fx) && scale <= 1.0)
    if (ysfx_real* retina = ysfx_find_var(fx, "gfx_ext_retina"))
      *retina = 1.0;
  const int pw = std::max(1, static_cast<int>(std::lround(reqW * scale)));
  const int ph = std::max(1, static_cast<int>(std::lround(reqH * scale)));
  if (pw != mBackW || ph != mBackH)
  {
    mBack.assign(static_cast<size_t>(pw) * ph, 0u);
    mBackW = pw;
    mBackH = ph;
    mForcePublish = true;
  }

  ysfx_gfx_config_t gc{};
  gc.user_data = this;
  gc.pixel_width = static_cast<uint32_t>(pw);
  gc.pixel_height = static_cast<uint32_t>(ph);
  gc.pixel_stride = static_cast<uint32_t>(pw) * 4u;
  gc.pixels = reinterpret_cast<uint8_t*>(mBack.data());
  gc.scale_factor = scale;
  gc.show_menu = &GfxRunner::ShowMenuCb;
  gc.set_cursor = &GfxRunner::SetCursorCb;
  gc.get_drop_file = &GfxRunner::GetDropFileCb;
  ysfx_gfx_setup(fx, &gc);

  for (const auto& k : in.keys)
    ysfx_gfx_add_key(fx, k.mods, k.key, k.press);
  ysfx_gfx_update_mouse(fx, in.mods,
                        static_cast<int32_t>(std::lround(in.mouseX * scale)),
                        static_cast<int32_t>(std::lround(in.mouseY * scale)),
                        in.buttons, in.wheel, in.hwheel);
  ysfx_gfx_set_window_state(fx, in.hasFocus, in.visible, in.mouseOver);

  bool dirty;
  {
    std::lock_guard<std::mutex> runLock(GlobalGfxRunMutex());
    dirty = ysfx_gfx_run(fx);
  }

  if (!dirty && !mForcePublish)
    return;
  mForcePublish = false;

  // LICE pixels are 0xAARRGGBB words (bytes B,G,R,A in memory on little
  // endian). Publish RGBA bytes with an opaque alpha, ready for a texture.
  std::lock_guard<std::mutex> l(mFrontMutex);
  mFront.resize(static_cast<size_t>(pw) * ph * 4);
  uint8_t* dst = mFront.data();
  for (uint32_t px : mBack)
  {
    *dst++ = static_cast<uint8_t>(px >> 16);
    *dst++ = static_cast<uint8_t>(px >> 8);
    *dst++ = static_cast<uint8_t>(px);
    *dst++ = 0xFF;
  }
  mFrontW = pw;
  mFrontH = ph;
  mLatestSerial.store(++mFrontSerial);
}

int32_t GfxRunner::ShowMenuCb(void* user, const char* spec, int32_t x, int32_t y)
{
  auto* self = static_cast<GfxRunner*>(user);
  std::unique_lock<std::mutex> l(self->mMutex);
  if (self->mStop || self->mInterrupted)
    return 0;
  const double s = self->mFrameScale > 0.0 ? self->mFrameScale : 1.0;
  self->mMenuReq.spec = spec ? spec : "";
  self->mMenuReq.x = static_cast<float>(x / s);
  self->mMenuReq.y = static_cast<float>(y / s);
  self->mMenuResult = 0;
  self->mMenuState = MenuState::Pending;
  self->mCv.wait(l, [self] {
    return self->mMenuState == MenuState::Answered || self->mStop || self->mInterrupted;
  });
  const int result = (self->mMenuState == MenuState::Answered) ? self->mMenuResult : 0;
  self->mMenuState = MenuState::None;
  return result;
}

void GfxRunner::SetCursorCb(void* user, int32_t cursor)
{
  static_cast<GfxRunner*>(user)->mCursor.store(cursor);
}

const char* GfxRunner::GetDropFileCb(void* user, int32_t index)
{
  auto* self = static_cast<GfxRunner*>(user);
  std::lock_guard<std::mutex> l(self->mDropMutex);
  if (index < 0)
  {
    self->mDropped.clear();
    return nullptr;
  }
  if (static_cast<size_t>(index) >= self->mDropped.size())
    return nullptr;
  self->mDropReturn = self->mDropped[static_cast<size_t>(index)];
  return self->mDropReturn.c_str();
}

} // namespace jsfx
