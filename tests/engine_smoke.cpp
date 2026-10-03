// Engine smoke test: everything except the iPlug2 layer, on any OS.
//   - unpack the embedded example, load + compile it
//   - MIDI note -> non-silent, finite audio
//   - host parameter -> slider, state save/load round trip incl. @serialize
//   - @gfx: frame rendering, knob drag -> automation, popup menu round trip
//   - hot reload keeps the state
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

static int gFailures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); ++gFailures; } } while (0)

static void SetEnv(const char* k, const std::string& v)
{
#if defined(_WIN32)
  _putenv_s(k, v.c_str());
#else
  setenv(k, v.c_str(), 1);
#endif
}

static int ParamIndex(const char* name)
{
  for (int i = 0; i < JSFX_NUM_PARAMS; ++i)
    if (std::string(kJsfxParams[i].name) == name) return i;
  return -1;
}

struct Block
{
  static constexpr uint32_t N = 256;
  std::vector<double> l = std::vector<double>(N), r = std::vector<double>(N);
  double* outs[2] = {l.data(), r.data()};
  double Peak() const { double p = 0; for (uint32_t i = 0; i < N; ++i) p = std::max(p, std::max(std::abs(l[i]), std::abs(r[i]))); return p; }
  bool Finite() const { for (uint32_t i = 0; i < N; ++i) if (!std::isfinite(l[i]) || !std::isfinite(r[i])) return false; return true; }
};

static bool Process(jsfx::Engine& e, Block& b)
{
  return e.Process(static_cast<const double* const*>(nullptr), b.outs, 0, 2, Block::N);
}

static bool WaitFrame(jsfx::GfxRunner& g, uint64_t& serial, int w, int h, jsfx::GfxInput in, int* pw = nullptr, int* ph = nullptr, uint32_t* centerPixel = nullptr)
{
  g.RequestFrame(w, h, 1.0, std::move(in));
  for (int i = 0; i < 400 && g.IsBusy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return g.ConsumeFrame(serial, [&](const uint8_t* rgba, int fw, int fh) {
    if (pw) *pw = fw;
    if (ph) *ph = fh;
    if (const char* dump = std::getenv("JSFX_SMOKE_DUMP"))   // debugging aid: write a .ppm
    {
      if (FILE* f = std::fopen(dump, "wb"))
      {
        std::fprintf(f, "P6\n%d %d\n255\n", fw, fh);
        for (int i = 0; i < fw * fh; ++i) std::fwrite(rgba + i * 4, 1, 3, f);
        std::fclose(f);
      }
    }
    if (centerPixel)
    {
      const uint8_t* p = rgba + (static_cast<size_t>(fh / 2) * fw + fw / 2) * 4;
      *centerPixel = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    }
  });
}

int main(int argc, char** argv)
{
  const std::string cache = argc > 1 ? argv[1] : "jsfx_test_cache";
  SetEnv("JSFX_CACHE_DIR", cache);

  // ------------------------------------------------------------ generator
  std::printf("generated: %s, %d params, io %s, gfx %d (%dx%d)\n", JSFX_DESC, JSFX_NUM_PARAMS, JSFX_CHANNEL_IO, JSFX_HAS_GFX, JSFX_GFX_WIDTH, JSFX_GFX_HEIGHT);
  CHECK(JSFX_NUM_PARAMS == 8);
  CHECK(JSFX_NUM_INPUTS == 0 && JSFX_NUM_OUTPUTS == 2);
  CHECK(JSFX_HAS_GFX == 1 && JSFX_GFX_WIDTH == 720 && JSFX_GFX_HEIGHT == 380);
  const int pWave = ParamIndex("Waveform"), pCutoff = ParamIndex("Cutoff (Hz)");
  CHECK(pWave == 0 && pCutoff == 1);
  CHECK(kJsfxParams[0].isEnum && kJsfxParams[0].numEnumNames == 4);
  CHECK(kJsfxParams[1].shape == 1);   // log
  // normalized mapping is monotonic and round-trips
  {
    const auto& p = kJsfxParams[pCutoff];
    const double n = jsfx::SliderToNormalized(p, 1000.0);
    CHECK(std::abs(n - 0.5) < 1e-6);   // :log=1000 -> centre
    CHECK(std::abs(jsfx::NormalizedToSlider(p, n) - 1000.0) < 1e-6);
  }

  // ------------------------------------------------------------ unpack + load
  std::string dir, err;
  CHECK(jsfx::ExtractBundle(jsfx_generated::kBundle, "TestVendor", "JsfxSynth", dir, err));
  std::printf("extracted to %s %s\n", dir.c_str(), err.c_str());
  CHECK(jsfx::ExtractBundle(jsfx_generated::kBundle, "TestVendor", "JsfxSynth", dir, err));   // second time: cached

  jsfx::Engine engine(kJsfxParams, JSFX_NUM_PARAMS);
  const bool loaded = engine.Load(dir + "/" + jsfx_generated::kBundle.mainFile, dir, dir + "/data");
  if (!loaded) std::fprintf(stderr, "load error: %s\n", engine.LastError().c_str());
  CHECK(loaded);
  CHECK(engine.HasGfx());
  engine.Prepare(48000.0, Block::N);

  // ------------------------------------------------------------ audio
  Block b;
  CHECK(Process(engine, b));
  CHECK(b.Peak() == 0.0);                          // silence before any note
  const uint8_t noteOn[3] = {0x90, 60, 110};
  engine.PushMidi(10, noteOn, 3);
  double peak = 0;
  for (int i = 0; i < 20; ++i) { CHECK(Process(engine, b)); CHECK(b.Finite()); peak = std::max(peak, b.Peak()); }
  std::printf("peak after note on: %f\n", peak);
  CHECK(peak > 0.01 && peak < 2.0);
  const uint8_t noteOff[3] = {0x80, 60, 0};
  engine.PushMidi(0, noteOff, 3);
  for (int i = 0; i < 400; ++i) Process(engine, b);
  CHECK(b.Peak() < 1e-4);                         // released

  // ------------------------------------------------------------ host -> slider
  engine.SetParamValue(pCutoff, 523.0);
  engine.SetParamValue(pWave, 2.0);
  Process(engine, b);
  int echoed = 0;
  engine.ConsumeSliderChanges([&](const jsfx::Engine::SliderChange&) { ++echoed; });
  CHECK(echoed == 0);                              // host changes are not echoed back

  // ------------------------------------------------------------ state
  const std::vector<uint8_t> blob = engine.SaveState();
  CHECK(blob.size() > 16);
  {
    jsfx::Engine other(kJsfxParams, JSFX_NUM_PARAMS);
    CHECK(other.Load(dir + "/" + jsfx_generated::kBundle.mainFile, dir, dir + "/data"));
    std::vector<double> values;
    CHECK(other.LoadState(blob.data(), blob.size(), &values));
    CHECK(values.size() == JSFX_NUM_PARAMS);
    CHECK(std::abs(values[pCutoff] - 523.0) < 1e-9);
    CHECK(values[pWave] == 2.0);
    std::lock_guard<std::mutex> l(other.GfxMutex());
    CHECK(ysfx_read_var(other.GfxFx(), "played_notes") == 1.0);   // @serialize restored
  }
  CHECK(!engine.LoadState(blob.data(), 7));        // truncated blob is rejected

  // ------------------------------------------------------------ @gfx
  {
    jsfx::GfxRunner gfx(engine);
    uint64_t serial = 0;
    int fw = 0, fh = 0;
    uint32_t center = 0;
    CHECK(WaitFrame(gfx, serial, 720, 380, {}, &fw, &fh, &center));
    std::printf("gfx frame %dx%d, centre pixel %06x\n", fw, fh, center);
    CHECK(fw == 720 && fh == 380);
    CHECK(center != 0);                            // something was painted

    // gfx_ext_retina=1 in the JSFX: a 2x screen gets a 2x framebuffer
    gfx.RequestFrame(720, 380, 2.0, {});
    for (int i = 0; i < 400 && gfx.IsBusy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    gfx.ConsumeFrame(serial, [&](const uint8_t*, int w, int h) { fw = w; fh = h; });
    std::printf("retina frame %dx%d\n", fw, fh);
    CHECK(fw == 1440 && fh == 760);
    WaitFrame(gfx, serial, 720, 380, {});           // back to 1x for the input tests below

    // drag the cutoff knob (centre at x=135,y=150) upwards
    jsfx::GfxInput in;
    in.mouseX = 135; in.mouseY = 150; in.buttons = ysfx_button_left; in.mouseOver = true;
    WaitFrame(gfx, serial, 720, 380, in);
    in.mouseY = 100;
    WaitFrame(gfx, serial, 720, 380, in);
    in.buttons = 0;
    WaitFrame(gfx, serial, 720, 380, in);
    Process(engine, b);
    bool automated = false;
    double newCutoff = 0;
    int gestures = 0;
    engine.ConsumeSliderChanges(
      [&](const jsfx::Engine::SliderChange& c) { if (c.paramIdx == pCutoff) { automated = c.automate; newCutoff = c.value; } },
      [&](int, bool) { ++gestures; });
    std::printf("cutoff after drag: %f (automate=%d)\n", newCutoff, automated);
    CHECK(automated && newCutoff > 523.0);
    {
      // a slider moved by @gfx must run @slider (fc is computed there)
      std::lock_guard<std::mutex> l(engine.GfxMutex());
      CHECK(std::abs(ysfx_read_var(engine.GfxFx(), "fc") - newCutoff) < 1e-6);
    }

    // right click on the cutoff knob -> gfx_showmenu -> "Reset to default"
    in.mouseY = 150;
    WaitFrame(gfx, serial, 720, 380, in);
    in.buttons = ysfx_button_right;
    gfx.RequestFrame(720, 380, 1.0, in);
    jsfx::GfxMenuRequest menu;
    bool gotMenu = false;
    for (int i = 0; i < 400 && !(gotMenu = gfx.TakeMenuRequest(menu)); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(gotMenu);
    std::printf("menu requested at %.0f,%.0f: \"%s\"\n", menu.x, menu.y, menu.spec.c_str());
    gfx.AnswerMenu(1);
    for (int i = 0; i < 400 && gfx.IsBusy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Process(engine, b);
    double resetValue = 0;
    engine.ConsumeSliderChanges([&](const jsfx::Engine::SliderChange& c) { if (c.paramIdx == pCutoff) resetValue = c.value; });
    CHECK(resetValue == 2400.0);

    // a state change while @gfx waits in a menu must not dead-lock
    in.buttons = 0;
    WaitFrame(gfx, serial, 720, 380, in);
    in.buttons = ysfx_button_right;
    gfx.RequestFrame(720, 380, 1.0, in);
    for (int i = 0; i < 400 && !gfx.TakeMenuRequest(menu); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(engine.LoadState(blob.data(), blob.size()));   // interrupts the menu
  }

  // ------------------------------------------------------------ hot reload
  engine.SetParamValue(pCutoff, 777.0);
  Process(engine, b);
  CHECK(engine.Reload());
  {
    const std::vector<uint8_t> after = engine.SaveState();
    jsfx::Engine probe(kJsfxParams, JSFX_NUM_PARAMS);
    probe.Load(dir + "/" + jsfx_generated::kBundle.mainFile, dir, dir + "/data");
    std::vector<double> values;
    CHECK(probe.LoadState(after.data(), after.size(), &values));
    CHECK(values.size() == JSFX_NUM_PARAMS && std::abs(values[pCutoff] - 777.0) < 1e-9);
  }
  engine.PushMidi(0, noteOn, 3);
  peak = 0;
  for (int i = 0; i < 10; ++i) { Process(engine, b); peak = std::max(peak, b.Peak()); }
  CHECK(peak > 0.01);

  // optional: engine_smoke <cache> --bench  -> realtime factor with 8 voices
  if (argc > 2 && std::string(argv[2]) == "--bench")
  {
    for (uint8_t n = 0; n < 8; ++n)
    {
      const uint8_t on[3] = {0x90, static_cast<uint8_t>(48 + n * 3), 100};
      engine.PushMidi(0, on, 3);
    }
    const int blocks = static_cast<int>(48000 * 20 / Block::N);   // 20 s of audio
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < blocks; ++i) Process(engine, b);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bench: 20 s of 8-voice audio in %.3f s (%.0fx realtime)\n", secs, 20.0 / secs);
  }

  std::printf(gFailures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", gFailures);
  return gFailures ? 1 : 0;
}
