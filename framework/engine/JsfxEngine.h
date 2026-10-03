// =============================================================================
//  JsfxEngine.h  -  host-agnostic runtime for ONE JSFX file
//
//  Wraps ysfx with the threading rules a plugin needs. No iPlug2 dependency,
//  so it is unit-testable and reusable.
//
//  Threads and locks
//  -----------------
//   audio thread  : Process()          try-locks mFxMutex (never blocks; a block
//                                      is rendered silent if the lock is busy)
//   gfx thread    : JsfxGfxRunner      locks mGfxMutex while running @gfx.
//                                      @gfx runs concurrently with @sample,
//                                      exactly like in REAPER.
//   main thread   : Load/Reload/       lock mGfxMutex THEN mFxMutex (blocking),
//                   SaveState/         so nothing else touches the VM while the
//                   LoadState/Prepare  state is swapped or serialized.
//
//  Parameters
//  ----------
//  Host parameters are a fixed table (ParamDef) generated at build time from
//  the JSFX sliders. Values cross threads through atomics + bit masks:
//    host -> JSFX : SetParamValue()            (any thread)  -> applied at the
//                                                              start of Process()
//    JSFX -> host : ConsumeSliderChanges()     (main thread) <- published at the
//                                                              end of Process()
// =============================================================================
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ysfx.h"

namespace jsfx {

// One host parameter == one JSFX slider. Emitted by the jsfx_meta generator.
struct ParamDef
{
  uint32_t slider;          // 0-based ysfx slider index (slider1 -> 0)
  const char* name;
  double def, min, max, inc;
  uint8_t shape;            // ysfx_slider_curve_t::shape (0 linear, 1 log, 2 sqr)
  double modifier;          // ysfx_slider_curve_t::modifier
  bool isEnum;
  bool visible;             // false for "-hidden" sliders
  const char* const* enumNames;
  uint32_t numEnumNames;

  ysfx_slider_curve_t Curve() const
  {
    ysfx_slider_curve_t c{};
    c.def = def; c.min = min; c.max = max; c.inc = inc;
    c.shape = shape; c.modifier = modifier;
    return c;
  }
};

class Engine
{
public:
  using MidiOutFunc = void (*)(void* user, uint32_t offset, const uint8_t* data, uint32_t size);
  using LogFunc = std::function<void(ysfx_log_level, const std::string&)>;

  Engine(const ParamDef* params, int numParams);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // ---------------------------------------------------------------- main thread
  /** Loads and compiles a JSFX. importRoot resolves `import`, dataRoot resolves
   *  file_open()/gfx_loadimg() names. Returns false and fills LastError() on failure
   *  (the previous effect, if any, keeps running). */
  bool Load(const std::string& jsfxPath, const std::string& importRoot, const std::string& dataRoot);
  /** Recompiles the same file, carrying sliders and @serialize data over. */
  bool Reload();
  bool IsLoaded() const { return mLoaded.load(); }
  std::string LastError() const;
  std::string FilePath() const;
  void SetLogFunc(LogFunc f) { mLogFunc = std::move(f); }

  /** Sample rate / block size; runs @init on the next block. */
  void Prepare(double sampleRate, uint32_t blockSize);

  /** Serializes sliders + @serialize data. Never called on the audio thread. */
  std::vector<uint8_t> SaveState();
  /** Restores a blob produced by SaveState(). On success, sliderValues (indexed
   *  like the ParamDef table) receives the restored values. */
  bool LoadState(const uint8_t* data, size_t size, std::vector<double>* paramValues = nullptr);

  bool HasGfx() const { return mHasGfx.load(); }
  /** @gfx's requested size ("@gfx 640 400"); 0 when unspecified. */
  void GetGfxSize(uint32_t& w, uint32_t& h) const { w = mGfxW.load(); h = mGfxH.load(); }
  int GetLatency() const { return mLatency.load(); }

  // ------------------------------------------------------------ parameters
  int NumParams() const { return static_cast<int>(mParams.size()); }
  const ParamDef& Param(int idx) const { return mParams[idx]; }
  /** Host changed a parameter (plain JSFX value). Any thread. */
  void SetParamValue(int paramIdx, double value);

  struct SliderChange { int paramIdx; double value; bool automate; };
  /** Delivers slider changes made by the JSFX itself since the last call.
   *  touchBegin/touchEnd report slider_automate() gestures. Main thread. */
  void ConsumeSliderChanges(const std::function<void(const SliderChange&)>& onChange,
                            const std::function<void(int paramIdx, bool begin)>& onGesture = {});

  // ------------------------------------------------------------ audio thread
  void SetTimeInfo(const ysfx_time_info_t& ti) { mTimeInfo = ti; }
  /** Queue an incoming MIDI message for the next Process() call. */
  void PushMidi(uint32_t offset, const uint8_t* data, uint32_t size);
  /** Renders one block. Returns false if the block was rendered silent. */
  bool Process(const float* const* ins, float* const* outs, uint32_t nIns, uint32_t nOuts, uint32_t nFrames, MidiOutFunc midiOut = nullptr, void* user = nullptr);
  bool Process(const double* const* ins, double* const* outs, uint32_t nIns, uint32_t nOuts, uint32_t nFrames, MidiOutFunc midiOut = nullptr, void* user = nullptr);

  // ------------------------------------------------------------ gfx thread
  /** Must be held while calling any ysfx_gfx_* function on GfxFx(). */
  std::mutex& GfxMutex() { return mGfxMutex; }
  /** Takes the gfx lock for one @gfx frame, but first steps aside while a
   *  main-thread operation (state save/load, reload, prepare) is waiting for
   *  it. Mutexes are not fair (macOS's default pthread mutex can let the gfx
   *  thread re-take the lock forever): this bounds that wait to one frame. */
  std::unique_lock<std::mutex> LockGfxForFrame();
  ysfx_t* GfxFx() { return mFx; }
  /** Registered by the gfx runner: wakes a @gfx blocked in a synchronous popup
   *  menu so the main thread can take the gfx lock without dead-locking. */
  void SetGfxInterruptor(std::function<void()> f);

private:
  struct ExclusiveLock;   // gfx + fx lock, in that order
  ysfx_t* CreateCompiled(const std::string& path, std::string& error);
  void Install(ysfx_t* fx);           // requires both locks
  template <typename T>
  bool ProcessT(const T* const* ins, T* const* outs, uint32_t nIns, uint32_t nOuts, uint32_t nFrames, MidiOutFunc midiOut, void* user);
  void ApplyPendingParams();
  void PublishSliders();
  void ReportLog(ysfx_log_level level, const char* msg);
  static void LogCallback(intptr_t user, ysfx_log_level level, const char* message);

  std::vector<ParamDef> mParams;
  std::array<int16_t, ysfx_max_sliders> mParamOfSlider{};

  // effect (guarded by mFxMutex for writers, see header comment)
  std::mutex mGfxMutex;
  std::mutex mFxMutex;
  ysfx_t* mFx = nullptr;
  std::string mPath, mImportRoot, mDataRoot;
  double mSampleRate = 44100.0;
  uint32_t mBlockSize = 512;
  std::atomic<bool> mLoaded{false};
  std::atomic<bool> mHasGfx{false};
  std::atomic<uint32_t> mGfxW{0}, mGfxH{0};
  std::atomic<int> mLatency{0};

  mutable std::mutex mErrorMutex;
  std::string mLastError;
  std::string mLoadLog;               // collected while compiling
  LogFunc mLogFunc;

  std::atomic<int> mExclusiveWaiters{0};   // main-thread ops waiting for the gfx lock
  std::mutex mInterruptMutex;
  std::function<void()> mGfxInterruptor;

  // host -> slider
  std::array<std::atomic<double>, ysfx_max_sliders> mParamTarget{};
  std::array<std::atomic<uint64_t>, ysfx_max_slider_groups> mPendingToSlider{};
  // slider -> host
  std::array<double, ysfx_max_sliders> mLastSeen{};                 // guarded by mFxMutex
  std::array<std::atomic<double>, ysfx_max_sliders> mPublished{};
  std::array<std::atomic<uint64_t>, ysfx_max_slider_groups> mChanged{};
  std::array<std::atomic<uint64_t>, ysfx_max_slider_groups> mAutomated{};
  std::array<std::atomic<uint64_t>, ysfx_max_slider_groups> mTouching{};
  std::array<uint64_t, ysfx_max_slider_groups> mTouchingSeen{};     // main thread only

  // audio thread scratch (pre-allocated, never resized on the audio thread)
  ysfx_time_info_t mTimeInfo{};
  struct MidiEvent { uint32_t offset, size, start; };
  std::vector<MidiEvent> mMidiEvents;
  std::vector<uint8_t> mMidiBytes;
  size_t mNumMidiEvents = 0, mNumMidiBytes = 0;
};

// -----------------------------------------------------------------------------
// Little helpers shared by the plugin layer
// -----------------------------------------------------------------------------
/** Maps a plain JSFX slider value to [0,1] using the slider's own curve. */
double SliderToNormalized(const ParamDef& p, double value);
double NormalizedToSlider(const ParamDef& p, double normalized);

} // namespace jsfx
