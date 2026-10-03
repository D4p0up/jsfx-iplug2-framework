// =============================================================================
//  JsfxPlugin.h  -  the one iPlug2 plugin class shared by every JSFX plugin
//
//  Nothing here is specific to a given JSFX: the parameter table, the I/O
//  layout, the UI size and the embedded files are generated at build time
//  (jsfx_meta.h / jsfx_params.inc / jsfx_embedded.cpp, see cmake/).
// =============================================================================
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "IPlug_include_in_plug_hdr.h"
#include "JsfxEngine.h"

using namespace iplug;
#if IPLUG_EDITOR
using namespace igraphics;
#endif

class JsfxPlugin final : public Plugin
{
public:
  JsfxPlugin(const InstanceInfo& info);
  ~JsfxPlugin() override;

  jsfx::Engine& Engine() { return *mEngine; }

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void ProcessSysEx(const ISysEx& msg) override;
  void OnReset() override;
#endif
  void OnParamChange(int paramIdx) override;
  void OnIdle() override;
  bool SerializeState(IByteChunk& chunk) const override;
  int UnserializeState(const IByteChunk& chunk, int startPos) override;

#if IPLUG_EDITOR
  /** Builds the UI on open, re-positions it on resize (overrides IGEditorDelegate). */
  void LayoutUI(IGraphics* pGraphics) override;
  bool OnHostRequestingSupportedViewConfiguration(int width, int height) override
  {
#if JSFX_RESIZABLE
    return ConstrainEditorResize(width, height);
#else
    return width == PLUG_WIDTH && height == PLUG_HEIGHT;   // fixed-size editor
#endif
  }
  void OnHostSelectedViewConfiguration(int width, int height) override;
#endif

private:
  void LoadJsfx();
  void ApplySliderChange(const jsfx::Engine::SliderChange& c);
  double ParamToSlider(int paramIdx) const;
  double SliderToParam(int paramIdx, double sliderValue) const;
  static void MidiOutCallback(void* user, uint32_t offset, const uint8_t* data, uint32_t size);

  std::unique_ptr<jsfx::Engine> mEngine;

  // Values we pushed to the host because the JSFX moved a slider. The host
  // usually echoes them back through OnParamChange(); forwarding the echo to
  // the JSFX would fight a slider the JSFX keeps moving (LFO, morphing...).
  static constexpr int kMaxParams = 256;
  std::array<std::atomic<double>, kMaxParams> mEchoValue{};
  std::array<std::atomic<bool>, kMaxParams> mEchoPending{};
  std::array<bool, kMaxParams> mInGesture{};        // main thread

#if JSFX_DEV_RELOAD
  // dev hot reload (config.h, JSFX_DEV_RELOAD=ON builds)
  std::string mDevSourcePath;
  long long mDevLastStamp = 0;
  std::chrono::steady_clock::time_point mDevLastCheck{};
#endif
};
