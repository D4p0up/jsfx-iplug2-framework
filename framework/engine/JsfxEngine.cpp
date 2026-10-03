#include "JsfxEngine.h"
#include "JsfxYsfxBridge.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

#if defined(_MSC_VER)
  #include <intrin.h>
#endif

namespace jsfx {

namespace {

constexpr uint32_t kStateMagic = 0x5846534A;   // "JSFX" little endian
constexpr uint32_t kStateVersion = 1;
constexpr size_t kMaxMidiEvents = 1024;
constexpr size_t kMaxMidiBytes = 64 * 1024;

inline uint8_t GroupOf(uint32_t slider) { return static_cast<uint8_t>(slider >> 6); }
inline uint64_t BitOf(uint32_t slider) { return uint64_t(1) << (slider & 63); }

template <typename F>
inline void ForEachBit(uint8_t group, uint64_t mask, F&& f)
{
  while (mask)
  {
#if defined(_MSC_VER)
    unsigned long bit;
    _BitScanForward64(&bit, mask);
#else
    const unsigned bit = static_cast<unsigned>(__builtin_ctzll(mask));
#endif
    mask &= mask - 1;
    f(static_cast<uint32_t>(group) * 64u + static_cast<uint32_t>(bit));
  }
}

// --- little-endian blob helpers ---------------------------------------------
void PutU32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void PutU64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void PutF64(std::vector<uint8_t>& b, double d) { uint64_t v; std::memcpy(&v, &d, 8); PutU64(b, v); }

struct Reader
{
  const uint8_t* p; size_t n, pos = 0;
  bool ok = true;
  uint64_t Get(int bytes)
  {
    if (pos + bytes > n) { ok = false; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= uint64_t(p[pos + i]) << (8 * i);
    pos += bytes;
    return v;
  }
  uint32_t U32() { return static_cast<uint32_t>(Get(4)); }
  uint64_t U64() { return Get(8); }
  double F64() { uint64_t v = Get(8); double d; std::memcpy(&d, &v, 8); return d; }
};

struct StateDeleter { void operator()(ysfx_state_t* s) const { ysfx_state_free(s); } };
using StatePtr = std::unique_ptr<ysfx_state_t, StateDeleter>;

} // namespace

// Gfx lock first, then fx lock. If @gfx holds its lock for long, it may be
// waiting in gfx_showmenu() for this very (main) thread to show the menu:
// after a short grace period the pending menu is cancelled to avoid a
// dead-lock (a normal @gfx frame finishes well within that time).
struct Engine::ExclusiveLock
{
  explicit ExclusiveLock(Engine& e) : engine(e)
  {
    // Announce ourselves: the gfx thread won't start another frame until we
    // own the gfx lock (see LockGfxForFrame).
    e.mExclusiveWaiters.fetch_add(1);
    gfx = std::unique_lock<std::mutex>(e.mGfxMutex, std::defer_lock);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (!gfx.try_lock())
    {
      if (std::chrono::steady_clock::now() > deadline)
      {
        {
          std::lock_guard<std::mutex> l(e.mInterruptMutex);
          if (e.mGfxInterruptor) e.mGfxInterruptor();
        }
        gfx.lock();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    e.mExclusiveWaiters.fetch_sub(1);
    e.mFxWaiters.fetch_add(1);          // audio thread skips blocks meanwhile
    fx = std::unique_lock<std::mutex>(e.mFxMutex);
    e.mFxWaiters.fetch_sub(1);
  }
  Engine& engine;
  std::unique_lock<std::mutex> gfx, fx;
};

// =============================================================================
Engine::Engine(const ParamDef* params, int numParams)
: mParams(params, params + std::max(0, numParams))
{
  mParamOfSlider.fill(-1);
  for (int i = 0; i < NumParams(); ++i)
  {
    const uint32_t s = mParams[i].slider;
    if (s < ysfx_max_sliders)
      mParamOfSlider[s] = static_cast<int16_t>(i);
    mParamTarget[std::min<uint32_t>(s, ysfx_max_sliders - 1)].store(mParams[i].def);
  }
  for (auto& a : mPendingToSlider) a.store(0);
  for (auto& a : mChanged) a.store(0);
  for (auto& a : mAutomated) a.store(0);
  for (auto& a : mTouching) a.store(0);
  for (auto& v : mPublished) v.store(0.0);
  mTouchingSeen.fill(0);
  mLastSeen.fill(0.0);

  mMidiEvents.resize(kMaxMidiEvents);
  mMidiBytes.resize(kMaxMidiBytes);
}

Engine::~Engine()
{
  {
    std::lock_guard<std::mutex> l(mInterruptMutex);
    mGfxInterruptor = nullptr;
  }
  std::lock_guard<std::mutex> g(mGfxMutex);
  std::lock_guard<std::mutex> f(mFxMutex);
  if (mFx) ysfx_free(mFx);
  mFx = nullptr;
}

std::unique_lock<std::mutex> Engine::LockGfxForFrame()
{
  while (mExclusiveWaiters.load() > 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return std::unique_lock<std::mutex>(mGfxMutex);
}

void Engine::SetGfxInterruptor(std::function<void()> f)
{
  std::lock_guard<std::mutex> l(mInterruptMutex);
  mGfxInterruptor = std::move(f);
}

std::string Engine::LastError() const
{
  std::lock_guard<std::mutex> l(mErrorMutex);
  return mLastError;
}

std::string Engine::FilePath() const
{
  std::lock_guard<std::mutex> l(mErrorMutex);
  return mPath;
}

void Engine::LogCallback(intptr_t user, ysfx_log_level level, const char* message)
{
  reinterpret_cast<Engine*>(user)->ReportLog(level, message);
}

void Engine::ReportLog(ysfx_log_level level, const char* msg)
{
  {
    std::lock_guard<std::mutex> l(mErrorMutex);
    if (level == ysfx_log_error)
    {
      if (!mLoadLog.empty()) mLoadLog += "\n";
      mLoadLog += msg;
    }
  }
  if (mLogFunc) mLogFunc(level, msg);
}

// -----------------------------------------------------------------------------
ysfx_t* Engine::CreateCompiled(const std::string& path, std::string& error)
{
  ysfx_config_t* config = ysfx_config_new();
  ysfx_register_builtin_audio_formats(config);
  if (!mImportRoot.empty()) ysfx_set_import_root(config, mImportRoot.c_str());
  if (!mDataRoot.empty()) ysfx_set_data_root(config, mDataRoot.c_str());
  ysfx_guess_file_roots(config, path.c_str());   // only fills what is still unset
  ysfx_set_log_reporter(config, &Engine::LogCallback);
  ysfx_set_user_data(config, reinterpret_cast<intptr_t>(this));

  ysfx_t* fx = ysfx_new(config);
  ysfx_config_free(config);                      // fx holds its own reference

  {
    std::lock_guard<std::mutex> l(mErrorMutex);
    mLoadLog.clear();
  }

  bool ok = ysfx_load_file(fx, path.c_str(), 0) && ysfx_compile(fx, 0);
  if (!ok)
  {
    std::lock_guard<std::mutex> l(mErrorMutex);
    error = mLoadLog.empty() ? ("Cannot load " + path) : mLoadLog;
    ysfx_free(fx);
    return nullptr;
  }
  ysfx_set_midi_capacity(fx, 1024, true);
  return fx;
}

void Engine::Install(ysfx_t* fx)
{
  if (mFx) ysfx_free(mFx);
  mFx = fx;
  ysfx_set_sample_rate(mFx, mSampleRate);
  ysfx_set_block_size(mFx, mBlockSize);
  ysfx_init(mFx);

  mHasGfx.store(ysfx_has_section(mFx, ysfx_section_gfx));
  uint32_t dim[2] = {0, 0};
  ysfx_get_gfx_dim(mFx, dim);
  mGfxW.store(dim[0]);
  mGfxH.store(dim[1]);

  // Everything is "already published": the next Process() only reports real changes.
  for (auto& p : mParams)
  {
    if (p.slider >= ysfx_max_sliders) continue;
    const double v = ysfx_slider_get_value(mFx, p.slider);
    mLastSeen[p.slider] = v;
    mPublished[p.slider].store(v);
  }
  for (auto& a : mPendingToSlider) a.store(0);
  mLoaded.store(true);
}

bool Engine::Load(const std::string& jsfxPath, const std::string& importRoot, const std::string& dataRoot)
{
  mImportRoot = importRoot;
  mDataRoot = dataRoot;

  std::string error;
  ysfx_t* fx = CreateCompiled(jsfxPath, error);   // slow part: outside the locks
  {
    std::lock_guard<std::mutex> l(mErrorMutex);
    mLastError = error;
    mPath = jsfxPath;
  }
  if (!fx) return false;

  ExclusiveLock lock(*this);
  // apply current host values (params set before the effect existed)
  for (auto& p : mParams)
    if (p.slider < ysfx_max_sliders)
      ysfx_slider_set_value(fx, p.slider, mParamTarget[p.slider].load(), true);
  Install(fx);
  return true;
}

bool Engine::Reload()
{
  const std::string path = FilePath();
  if (path.empty()) return false;

  std::string error;
  ysfx_t* fx = CreateCompiled(path, error);
  {
    std::lock_guard<std::mutex> l(mErrorMutex);
    mLastError = error;
  }
  if (!fx) return false;

  ExclusiveLock lock(*this);
  if (mFx && ysfx_is_compiled(mFx))
  {
    StatePtr state(ysfx_save_state(mFx));
    if (state) ysfx_load_state(fx, state.get());
  }
  Install(fx);
  return true;
}

void Engine::Prepare(double sampleRate, uint32_t blockSize)
{
  ExclusiveLock lock(*this);
  mSampleRate = sampleRate > 0 ? sampleRate : 44100.0;
  mBlockSize = blockSize > 0 ? blockSize : 512;
  if (mFx)
  {
    ysfx_set_sample_rate(mFx, mSampleRate);
    ysfx_set_block_size(mFx, mBlockSize);
    ysfx_init(mFx);
  }
}

// -----------------------------------------------------------------------------
// State blob:  u32 magic | u32 version | u32 n | n x (u32 slider, f64 value)
//              | u64 size | size bytes of @serialize data
// -----------------------------------------------------------------------------
std::vector<uint8_t> Engine::SaveState()
{
  std::vector<uint8_t> blob;
  ExclusiveLock lock(*this);

  // Host values not yet seen by the audio thread are part of the state too.
  ApplyPendingParams();

  StatePtr state(mFx && ysfx_is_compiled(mFx) ? ysfx_save_state(mFx) : nullptr);
  PutU32(blob, kStateMagic);
  PutU32(blob, kStateVersion);
  if (state)
  {
    PutU32(blob, state->slider_count);
    for (uint32_t i = 0; i < state->slider_count; ++i)
    {
      PutU32(blob, state->sliders[i].index);
      PutF64(blob, state->sliders[i].value);
    }
    PutU64(blob, state->data_size);
    blob.insert(blob.end(), state->data, state->data + state->data_size);
  }
  else
  {
    // no effect: store the host values so nothing is lost
    PutU32(blob, static_cast<uint32_t>(mParams.size()));
    for (auto& p : mParams)
    {
      PutU32(blob, p.slider);
      PutF64(blob, mParamTarget[std::min<uint32_t>(p.slider, ysfx_max_sliders - 1)].load());
    }
    PutU64(blob, 0);
  }
  return blob;
}

bool Engine::LoadState(const uint8_t* data, size_t size, std::vector<double>* paramValues)
{
  Reader r{data, size};
  if (r.U32() != kStateMagic || !r.ok) return false;
  const uint32_t version = r.U32();
  if (!r.ok || version > kStateVersion) return false;

  const uint32_t n = r.U32();
  if (!r.ok || n > ysfx_max_sliders) return false;
  std::vector<ysfx_state_slider_t> sliders(n);
  for (auto& s : sliders)
  {
    s.index = r.U32();
    s.value = r.F64();
  }
  const uint64_t dataSize = r.U64();
  if (!r.ok || dataSize > size - r.pos) return false;
  std::vector<uint8_t> serialized(data + r.pos, data + r.pos + dataSize);

  ExclusiveLock lock(*this);

  // host-side values first (valid even if the effect failed to compile)
  for (auto& s : sliders)
    if (s.index < ysfx_max_sliders)
      mParamTarget[s.index].store(s.value);
  for (auto& a : mPendingToSlider) a.store(0);

  if (mFx && ysfx_is_compiled(mFx))
  {
    ysfx_state_t st{};
    st.sliders = sliders.data();
    st.slider_count = n;
    st.data = serialized.data();
    st.data_size = serialized.size();
    ysfx_load_state(mFx, &st);
    for (auto& p : mParams)
    {
      if (p.slider >= ysfx_max_sliders) continue;
      const double v = ysfx_slider_get_value(mFx, p.slider);
      mParamTarget[p.slider].store(v);
      mLastSeen[p.slider] = v;
      mPublished[p.slider].store(v);
    }
  }

  if (paramValues)
  {
    paramValues->resize(mParams.size());
    for (size_t i = 0; i < mParams.size(); ++i)
      (*paramValues)[i] = mParamTarget[std::min<uint32_t>(mParams[i].slider, ysfx_max_sliders - 1)].load();
  }
  return true;
}

// -----------------------------------------------------------------------------
// Parameters
// -----------------------------------------------------------------------------
void Engine::SetParamValue(int paramIdx, double value)
{
  if (paramIdx < 0 || paramIdx >= NumParams()) return;
  const uint32_t s = mParams[paramIdx].slider;
  if (s >= ysfx_max_sliders) return;
  mParamTarget[s].store(value);
  mPendingToSlider[GroupOf(s)].fetch_or(BitOf(s));
}

void Engine::ApplyPendingParams()   // fx lock held
{
  if (!mFx) return;
  for (uint8_t g = 0; g < ysfx_max_slider_groups; ++g)
  {
    const uint64_t mask = mPendingToSlider[g].exchange(0);
    ForEachBit(g, mask, [&](uint32_t s) {
      double v = mParamTarget[s].load();
      // normalized round trips lose a little precision; snap near-integers
      const double r = std::round(v);
      if (std::abs(r - v) < 1e-6) v = (r == 0.0) ? 0.0 : r;
      if (v != ysfx_slider_get_value(mFx, s))
        ysfx_slider_set_value(mFx, s, v, true);   // runs @slider before next @block
      mLastSeen[s] = v;                            // host originated: don't echo back
      mPublished[s].store(v);
    });
  }
}

void Engine::PublishSliders()   // fx lock held, after processing
{
  for (uint8_t g = 0; g < ysfx_max_slider_groups; ++g)
  {
    const uint64_t automated = ysfx_fetch_slider_automations(mFx, g);
    const uint64_t touches = ysfx_fetch_slider_touches(mFx, g);
    mTouching[g].store(touches);
    if (automated)
      mAutomated[g].fetch_or(automated);
  }

  uint64_t changed[ysfx_max_slider_groups] = {};
  for (auto& p : mParams)
  {
    const uint32_t s = p.slider;
    if (s >= ysfx_max_sliders) continue;
    const double v = ysfx_slider_get_value(mFx, s);
    if (v != mLastSeen[s])
    {
      mLastSeen[s] = v;
      mPublished[s].store(v);
      changed[GroupOf(s)] |= BitOf(s);
    }
  }
  for (uint8_t g = 0; g < ysfx_max_slider_groups; ++g)
    if (changed[g])
      mChanged[g].fetch_or(changed[g]);
}

void Engine::ConsumeSliderChanges(const std::function<void(const SliderChange&)>& onChange,
                                  const std::function<void(int, bool)>& onGesture)
{
  for (uint8_t g = 0; g < ysfx_max_slider_groups; ++g)
  {
    const uint64_t automated = mAutomated[g].exchange(0);
    const uint64_t changed = mChanged[g].exchange(0) | automated;
    const uint64_t touching = mTouching[g].load();
    const uint64_t began = touching & ~mTouchingSeen[g];
    const uint64_t ended = mTouchingSeen[g] & ~touching;
    mTouchingSeen[g] = touching;

    if (onGesture)
      ForEachBit(g, began, [&](uint32_t s) { if (mParamOfSlider[s] >= 0) onGesture(mParamOfSlider[s], true); });

    ForEachBit(g, changed, [&](uint32_t s) {
      const int p = mParamOfSlider[s];
      if (p < 0) return;
      const double v = mPublished[s].load();
      mParamTarget[s].store(v);
      onChange({p, v, (automated & BitOf(s)) != 0});
    });

    if (onGesture)
      ForEachBit(g, ended, [&](uint32_t s) { if (mParamOfSlider[s] >= 0) onGesture(mParamOfSlider[s], false); });
  }
}

// -----------------------------------------------------------------------------
// Audio
// -----------------------------------------------------------------------------
void Engine::PushMidi(uint32_t offset, const uint8_t* data, uint32_t size)
{
  if (!size || mNumMidiEvents >= mMidiEvents.size() || mNumMidiBytes + size > mMidiBytes.size())
    return;   // overflow: drop
  std::memcpy(mMidiBytes.data() + mNumMidiBytes, data, size);
  mMidiEvents[mNumMidiEvents++] = {offset, size, static_cast<uint32_t>(mNumMidiBytes)};
  mNumMidiBytes += size;
}

template <typename T>
bool Engine::ProcessT(const T* const* ins, T* const* outs, uint32_t nIns, uint32_t nOuts, uint32_t nFrames, MidiOutFunc midiOut, void* user)
{
  // Never wait. Also yield to a main-thread operation that is waiting for the
  // lock: hosts can call us back to back (offline bounce), and an unfair mutex
  // (macOS) would otherwise let this thread re-take it forever.
  std::unique_lock<std::mutex> lock(mFxMutex, std::defer_lock);
  if (mFxWaiters.load() == 0)
    lock.try_lock();
  if (!lock.owns_lock() || !mFx)
  {
    for (uint32_t c = 0; c < nOuts; ++c)
      std::fill(outs[c], outs[c] + nFrames, T(0));
    if (!lock.owns_lock())
    {
      // keep queued MIDI (note-offs!) for the next block, at its start
      for (size_t i = 0; i < mNumMidiEvents; ++i) mMidiEvents[i].offset = 0;
    }
    else
    {
      mNumMidiEvents = mNumMidiBytes = 0;
    }
    return false;
  }

  // Sliders moved by @gfx since the previous block: run @slider, as the
  // JSFX expects after sliderchange(). Only a flag is raised: no slider value
  // is written here, so nothing can collide with @gfx writing the same slider.
  for (const auto& p : mParams)
  {
    if (p.slider < ysfx_max_sliders && ysfx_slider_get_value(mFx, p.slider) != mLastSeen[p.slider])
    {
      YsfxRequestSliderSection(mFx);
      break;
    }
  }

  ApplyPendingParams();
  ysfx_set_time_info(mFx, &mTimeInfo);

  for (size_t i = 0; i < mNumMidiEvents; ++i)
  {
    const MidiEvent& e = mMidiEvents[i];
    ysfx_midi_event_t ev{};
    ev.bus = 0;
    ev.offset = std::min(e.offset, nFrames ? nFrames - 1 : 0);
    ev.size = e.size;
    ev.data = mMidiBytes.data() + e.start;
    ysfx_send_midi(mFx, &ev);
  }
  mNumMidiEvents = mNumMidiBytes = 0;

  if constexpr (sizeof(T) == sizeof(double))
    ysfx_process_double(mFx, ins, outs, nIns, nOuts, nFrames);
  else
    ysfx_process_float(mFx, ins, outs, nIns, nOuts, nFrames);

  ysfx_midi_event_t out;
  while (ysfx_receive_midi(mFx, &out))
    if (midiOut) midiOut(user, out.offset, out.data, out.size);

  PublishSliders();
  mLatency.store(static_cast<int>(std::lround(ysfx_get_pdc_delay(mFx))));
  return true;
}

bool Engine::Process(const float* const* ins, float* const* outs, uint32_t nIns, uint32_t nOuts, uint32_t nFrames, MidiOutFunc midiOut, void* user)
{
  return ProcessT<float>(ins, outs, nIns, nOuts, nFrames, midiOut, user);
}

bool Engine::Process(const double* const* ins, double* const* outs, uint32_t nIns, uint32_t nOuts, uint32_t nFrames, MidiOutFunc midiOut, void* user)
{
  return ProcessT<double>(ins, outs, nIns, nOuts, nFrames, midiOut, user);
}

// -----------------------------------------------------------------------------
double SliderToNormalized(const ParamDef& p, double value)
{
  if (p.max == p.min) return 0.0;
  if (p.isEnum)
    return std::clamp((value - p.min) / (p.max - p.min), 0.0, 1.0);
  const ysfx_slider_curve_t c = p.Curve();
  return std::clamp(static_cast<double>(ysfx_ysfx_value_to_normalized(value, &c)), 0.0, 1.0);
}

double NormalizedToSlider(const ParamDef& p, double normalized)
{
  normalized = std::clamp(normalized, 0.0, 1.0);
  if (p.isEnum)
    return std::round(p.min + normalized * (p.max - p.min));
  const ysfx_slider_curve_t c = p.Curve();
  return ysfx_normalized_to_ysfx_value(normalized, &c);
}

} // namespace jsfx
