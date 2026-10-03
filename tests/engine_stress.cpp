// Concurrency stress: audio thread, @gfx thread and "main thread" state
// operations all at once, like a host recalling presets while the user plays
// and moves knobs. Fails on crash, dead-lock (watchdog) or silent audio.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "JsfxEmbedded.h"
#include "JsfxEngine.h"
#include "JsfxGfxRunner.h"
#include "jsfx_meta.h"
#include "jsfx_params.inc"

namespace jsfx_generated { extern const jsfx::EmbeddedBundle kBundle; }

int main(int argc, char** argv)
{
  const std::string cache = argc > 1 ? argv[1] : "jsfx_test_cache";
  const int mask = argc > 2 ? std::atoi(argv[2]) : 7;   // 1 gfx, 2 state ops, 4 param changes
#if defined(_WIN32)
  _putenv_s("JSFX_CACHE_DIR", cache.c_str());
#else
  setenv("JSFX_CACHE_DIR", cache.c_str(), 1);
#endif
  std::string dir, err;
  if (!jsfx::ExtractBundle(jsfx_generated::kBundle, "TestVendor", "JsfxSynth", dir, err)) return 1;

  jsfx::Engine engine(kJsfxParams, JSFX_NUM_PARAMS);
  if (!engine.Load(dir + "/" + jsfx_generated::kBundle.mainFile, dir, dir + "/data")) return 1;
  engine.Prepare(48000.0, 128);

  std::atomic<bool> stop{false};
  std::atomic<long> blocks{0}, rendered{0}, frames{0}, stateOps{0};
  std::atomic<double> peak{0.0};

  std::thread audio([&] {
    std::vector<float> l(128), r(128);
    float* outs[2] = {l.data(), r.data()};
    uint8_t note = 48;
    while (!stop)
    {
      if (blocks % 50 == 0)
      {
        const uint8_t on[3] = {0x90, note, 100}, off[3] = {0x80, static_cast<uint8_t>(note - 12), 0};
        engine.PushMidi(3, on, 3);
        engine.PushMidi(5, off, 3);
        note = note < 72 ? note + 1 : 48;
      }
      if (engine.Process(static_cast<const float* const*>(nullptr), outs, 0, 2, 128)) ++rendered;
      for (float v : l) if (std::abs(v) > peak.load()) peak.store(std::abs(v));
      if ((mask & 4) && (blocks & 7) == 0) engine.SetParamValue(1, 200.0 + (blocks % 1000) * 10.0);
      ++blocks;
    }
  });

  jsfx::GfxRunner gfx(engine);
  std::thread ui([&] {
    uint64_t serial = 0;
    float y = 150;
    while (!stop)
    {
      jsfx::GfxInput in;
      in.mouseX = 135; in.mouseY = y; in.mouseOver = true;
      in.buttons = (frames % 40 < 30) ? ysfx_button_left : 0;   // drag the cutoff knob
      if (frames % 97 == 0) in.buttons = ysfx_button_right;     // opens gfx_showmenu
      y = (y < 100) ? 150 : y - 1;
      if (mask & 1) gfx.RequestFrame(720, 380, (frames % 200 < 100) ? 1.0 : 2.0, in);
      jsfx::GfxMenuRequest menu;
      if (gfx.TakeMenuRequest(menu)) gfx.AnswerMenu((frames % 2) ? 1 : 0);
      gfx.ConsumeFrame(serial, [](const uint8_t*, int, int) {});
      engine.ConsumeSliderChanges([](const jsfx::Engine::SliderChange&) {});
      ++frames;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  std::atomic<int> hostStep{0};          // which call the host thread is in
  std::atomic<double> slowestOpMs{0.0};
  std::thread host([&] {
    while (!stop)
    {
      if (mask & 2)
      {
        const auto t = std::chrono::steady_clock::now();
        hostStep = 1;
        const auto blob = engine.SaveState();
        hostStep = 2;
        engine.LoadState(blob.data(), blob.size());
        hostStep = 3;
        if (stateOps % 10 == 0) engine.Reload();
        hostStep = 4;
        if (stateOps % 15 == 0) engine.Prepare(stateOps % 30 ? 44100.0 : 48000.0, 128);
        hostStep = 0;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        if (ms > slowestOpMs.load()) slowestOpMs.store(ms);
      }
      ++stateOps;
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
  });

  // watchdog: every thread must keep making progress
  const auto t0 = std::chrono::steady_clock::now();
  long lastB = -1, lastF = -1, lastS = -1;
  bool stalled = false;
  while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4))
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    if (blocks == lastB || frames == lastF || stateOps == lastS) { stalled = true; break; }
    lastB = blocks; lastF = frames; lastS = stateOps;
  }
  stop = true;
  if (stalled)
  {
    static const char* const kSteps[] = {"idle", "SaveState", "LoadState", "Reload", "Prepare"};
    std::fprintf(stderr, "DEAD-LOCK: blocks=%ld%s frames=%ld%s stateOps=%ld%s (host thread in %s), gfx busy=%d\n",
                 blocks.load(), blocks == lastB ? " [STALLED]" : "",
                 frames.load(), frames == lastF ? " [STALLED]" : "",
                 stateOps.load(), stateOps == lastS ? " [STALLED]" : "",
                 kSteps[hostStep.load()], gfx.IsBusy() ? 1 : 0);
    std::_Exit(1);
  }
  audio.join(); ui.join(); host.join();

  std::printf("blocks %ld (rendered %ld), gfx frames %ld, state ops %ld (slowest %.1f ms), peak %.3f\n",
              blocks.load(), rendered.load(), frames.load(), stateOps.load(), slowestOpMs.load(), peak.load());
  // Blocks that arrive while a state operation holds (or waits for) the engine
  // are rendered silent by design: the audio thread never waits. This loop runs
  // unpaced, far faster than real time, so the silent/rendered ratio means
  // nothing; require real audio, real state ops, and bounded state-op latency.
  const bool ok = rendered > 1000 && peak > 0.01 && peak < 4.0 && frames > 20 && stateOps > 5 &&
                  slowestOpMs.load() < 750.0;
  std::printf(ok ? "stress OK\n" : "stress FAILED\n");
  return ok ? 0 : 1;
}
