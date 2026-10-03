#include "JsfxPlugin.h"
#include "IPlug_include_in_plug_src.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#include "JsfxEmbedded.h"
#include "jsfx_params.inc"     // kJsfxParams[] (generated)

#if IPLUG_EDITOR
  #include "IControls.h"
  #include "JsfxGfxControl.h"
#endif

namespace jsfx_generated {
extern const jsfx::EmbeddedBundle kBundle;   // jsfx_embedded.cpp
extern const jsfx::EmbeddedBundle kFont;     // jsfx_font.cpp (Roboto, for the generic UI)
}

namespace {

// IParam shape that reuses the JSFX slider curve (linear / :log= / :sqr=), so
// host automation lanes map exactly like REAPER's.
struct JsfxShape : public IParam::Shape
{
  explicit JsfxShape(const jsfx::ParamDef* d) : def(d) {}
  Shape* Clone() const override { return new JsfxShape(*this); }
  IParam::EDisplayType GetDisplayType() const override
  {
    return def->shape == 1 ? IParam::kDisplayLog : def->shape == 2 ? IParam::kDisplaySquared : IParam::kDisplayLinear;
  }
  double NormalizedToValue(double v, const IParam&) const override { return jsfx::NormalizedToSlider(*def, v); }
  double ValueToNormalized(double v, const IParam&) const override { return jsfx::SliderToNormalized(*def, v); }
  const jsfx::ParamDef* def;
};

bool IsPlainEnum(const jsfx::ParamDef& d)
{
  return d.isEnum && d.numEnumNames > 0 && d.min == 0.0 && d.inc == 1.0 &&
         d.max == static_cast<double>(d.numEnumNames - 1);
}

#if IPLUG_EDITOR
enum ECtrlTags { kCtrlTagStatus = 0, kCtrlTagGfx, kCtrlTagTitle };

// Generic UI (when the JSFX has no @gfx): knobs on a grid. Keep these numbers
// in sync with tools/jsfx_meta.cpp, which sizes the window from them.
constexpr int kKnobsPerRow = 6;
constexpr float kCellW = 110.f, kCellH = 120.f, kHeaderH = 60.f, kMargin = 20.f;
const IColor kBgColor(255, 28, 28, 34);
const IColor kAccent(255, 242, 191, 77);
#endif

} // namespace

// =============================================================================
JsfxPlugin::JsfxPlugin(const InstanceInfo& info)
: Plugin(info, MakeConfig(JSFX_NUM_PARAMS, 0))
{
  for (auto& a : mEchoValue) a.store(0.0);
  for (auto& a : mEchoPending) a.store(false);
  mInGesture.fill(false);

  // ---------------------------------------------------------- parameters
  for (int i = 0; i < JSFX_NUM_PARAMS; ++i)
  {
    const jsfx::ParamDef& d = kJsfxParams[i];
    IParam* p = GetParam(i);
    const char* group = d.visible ? "" : "Hidden";
    if (IsPlainEnum(d))
    {
      p->InitEnum(d.name, static_cast<int>(d.def), static_cast<int>(d.numEnumNames), "", IParam::kFlagsNone, group);
      for (uint32_t e = 0; e < d.numEnumNames; ++e)
        p->SetDisplayText(static_cast<double>(e), d.enumNames[e]);
    }
    else
    {
      const double step = d.inc > 0.0 ? d.inc : (d.max - d.min) / 10000.0;
      p->InitDouble(d.name, d.def, d.min, d.max, step > 0.0 ? step : 0.001, "", IParam::kFlagsNone, group, JsfxShape(&d));
    }
  }

  mEngine = std::make_unique<jsfx::Engine>(kJsfxParams, JSFX_NUM_PARAMS);
  LoadJsfx();

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };
#endif
}

JsfxPlugin::~JsfxPlugin() = default;

void JsfxPlugin::LoadJsfx()
{
  std::string dir, error;

#if JSFX_DEV_RELOAD
  // Development build: run the JSFX straight from the source tree and
  // recompile it whenever it is saved (see OnIdle).
  {
    std::error_code ec;
    const std::string src = std::string(JSFX_DEV_SOURCE_DIR) + "/" + JSFX_MAIN_FILE;
    if (std::filesystem::exists(std::filesystem::u8path(src), ec))
    {
      mDevSourcePath = src;
      mDevLastStamp = std::filesystem::last_write_time(std::filesystem::u8path(src), ec).time_since_epoch().count();
      if (!mEngine->Load(src, JSFX_DEV_SOURCE_DIR, std::string(JSFX_DEV_SOURCE_DIR) + "/data"))
        DBGMSG("JSFX: %s\n", mEngine->LastError().c_str());
      return;
    }
  }
#endif

  if (!jsfx::ExtractBundle(jsfx_generated::kBundle, BUNDLE_MFR, PLUG_NAME, dir, error))
  {
    DBGMSG("JSFX: cannot unpack the embedded JSFX: %s\n", error.c_str());
    return;
  }
  if (!mEngine->Load(dir + "/" + jsfx_generated::kBundle.mainFile, dir, dir + "/data"))
    DBGMSG("JSFX: %s\n", mEngine->LastError().c_str());
}

// ---------------------------------------------------------------- conversions
double JsfxPlugin::ParamToSlider(int paramIdx) const
{
  const jsfx::ParamDef& d = kJsfxParams[paramIdx];
  const double v = GetParam(paramIdx)->Value();
  return IsPlainEnum(d) ? std::round(v) : v;
}

double JsfxPlugin::SliderToParam(int /*paramIdx*/, double sliderValue) const
{
  return sliderValue;   // same units: the IParam range *is* the slider range
}

// =============================================================================
// DSP
// =============================================================================
#if IPLUG_DSP
void JsfxPlugin::OnReset()
{
  mEngine->Prepare(GetSampleRate(), static_cast<uint32_t>(GetBlockSize()));
}

void JsfxPlugin::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  ysfx_time_info_t ti{};
  ti.tempo = GetTempo() > 0.0 ? GetTempo() : 120.0;
  ti.playback_state = GetTransportIsRunning() ? ysfx_playback_playing : ysfx_playback_stopped;
  ti.time_position = GetSampleRate() > 0.0 ? GetSamplePos() / GetSampleRate() : 0.0;
  ti.beat_position = GetPPQPos();
  int num = 4, den = 4;
  GetTimeSig(num, den);
  ti.time_signature[0] = static_cast<uint32_t>(num > 0 ? num : 4);
  ti.time_signature[1] = static_cast<uint32_t>(den > 0 ? den : 4);
  mEngine->SetTimeInfo(ti);

  const uint32_t nIns = static_cast<uint32_t>(std::min(MaxNChannels(ERoute::kInput), JSFX_NUM_INPUTS));
  const uint32_t nOuts = static_cast<uint32_t>(MaxNChannels(ERoute::kOutput));
  mEngine->Process(const_cast<const sample* const*>(inputs), outputs, nIns, nOuts,
                   static_cast<uint32_t>(nFrames), &JsfxPlugin::MidiOutCallback, this);
}

void JsfxPlugin::ProcessMidiMsg(const IMidiMsg& msg)
{
  const uint8_t data[3] = {msg.mStatus, msg.mData1, msg.mData2};
  uint32_t size = 3;
  switch (msg.mStatus & 0xF0)
  {
    case 0xC0: case 0xD0: size = 2; break;              // program change, channel pressure
    case 0xF0: size = (msg.mStatus == 0xF2) ? 3 : (msg.mStatus == 0xF1 || msg.mStatus == 0xF3) ? 2 : 1; break;
    default: break;
  }
  mEngine->PushMidi(static_cast<uint32_t>(std::max(0, msg.mOffset)), data, size);
}

void JsfxPlugin::ProcessSysEx(const ISysEx& msg)
{
  if (msg.mData && msg.mSize > 0)
    mEngine->PushMidi(static_cast<uint32_t>(std::max(0, msg.mOffset)), msg.mData, static_cast<uint32_t>(msg.mSize));
}

void JsfxPlugin::MidiOutCallback(void* user, uint32_t offset, const uint8_t* data, uint32_t size)
{
  auto* self = static_cast<JsfxPlugin*>(user);
  if (!size) return;
  if (size <= 3 && data[0] != 0xF0)
  {
    IMidiMsg msg(static_cast<int>(offset), data[0], size > 1 ? data[1] : 0, size > 2 ? data[2] : 0);
    self->SendMidiMsg(msg);
  }
  else
  {
    ISysEx sysex(static_cast<int>(offset), data, static_cast<int>(size));
    self->SendSysEx(sysex);
  }
}
#endif // IPLUG_DSP

// =============================================================================
// Parameters
// =============================================================================
void JsfxPlugin::OnParamChange(int paramIdx)
{
  if (paramIdx < 0 || paramIdx >= JSFX_NUM_PARAMS)
    return;
  if (mEchoPending[paramIdx].load())
  {
    if (std::abs(GetParam(paramIdx)->GetNormalized() - mEchoValue[paramIdx].load()) < 1e-6)
    {
      mEchoPending[paramIdx].store(false);
      return;   // our own value coming back from the host
    }
    mEchoPending[paramIdx].store(false);
  }
  mEngine->SetParamValue(paramIdx, ParamToSlider(paramIdx));
}

void JsfxPlugin::ApplySliderChange(const jsfx::Engine::SliderChange& c)
{
  IParam* p = GetParam(c.paramIdx);
  p->Set(SliderToParam(c.paramIdx, c.value));
  const double norm = p->GetNormalized();

  if (c.automate)
  {
    // the JSFX called slider_automate(): record it like a user gesture
    mEchoValue[c.paramIdx].store(norm);
    mEchoPending[c.paramIdx].store(true);
    if (!mInGesture[c.paramIdx]) BeginInformHostOfParamChange(c.paramIdx);
    InformHostOfParamChange(c.paramIdx, norm);
    if (!mInGesture[c.paramIdx]) EndInformHostOfParamChange(c.paramIdx);
  }
  // refresh controls bound to this parameter (generic UI)
  SendParameterValueFromDelegate(c.paramIdx, norm, true);
}

void JsfxPlugin::OnIdle()
{
  // JSFX -> host
  mEngine->ConsumeSliderChanges(
    [this](const jsfx::Engine::SliderChange& c) { ApplySliderChange(c); },
    [this](int paramIdx, bool begin) {
      if (begin == mInGesture[paramIdx]) return;
      mInGesture[paramIdx] = begin;
      begin ? BeginInformHostOfParamChange(paramIdx) : EndInformHostOfParamChange(paramIdx);
    });

  // pdc_delay
  const int latency = mEngine->GetLatency();
  if (latency != GetLatency() && latency >= 0)
    SetLatency(latency);

#if JSFX_DEV_RELOAD
  const auto now = std::chrono::steady_clock::now();
  if (!mDevSourcePath.empty() && now - mDevLastCheck > std::chrono::milliseconds(500))
  {
    mDevLastCheck = now;
    std::error_code ec;
    const long long stamp = std::filesystem::last_write_time(std::filesystem::u8path(mDevSourcePath), ec).time_since_epoch().count();
    if (!ec && stamp != mDevLastStamp)
    {
      mDevLastStamp = stamp;
      if (mEngine->Reload())
        DBGMSG("JSFX: reloaded %s\n", mDevSourcePath.c_str());
      // on failure the previous build keeps running and the UI shows the error
    }
  }
#endif
}

// =============================================================================
// State: [int32 size][engine blob]. The blob holds every slider value plus the
// JSFX @serialize data, so presets/projects restore the complete instrument.
// =============================================================================
bool JsfxPlugin::SerializeState(IByteChunk& chunk) const
{
  const std::vector<uint8_t> blob = mEngine->SaveState();
  const int32_t size = static_cast<int32_t>(blob.size());
  chunk.Put(&size);
  if (size > 0)
    chunk.PutBytes(blob.data(), size);
  return true;
}

int JsfxPlugin::UnserializeState(const IByteChunk& chunk, int startPos)
{
  int32_t size = 0;
  int pos = chunk.Get(&size, startPos);
  if (pos < 0 || size < 0 || pos + size > chunk.Size())
    return 0;   // not our format / truncated

  std::vector<uint8_t> blob(static_cast<size_t>(size));
  if (size > 0)
    pos = chunk.GetBytes(blob.data(), size, pos);

  std::vector<double> values;
  if (mEngine->LoadState(blob.data(), blob.size(), &values))
  {
    // Host-visible parameters must match the restored sliders: the host
    // calls OnParamReset()/OnRestoreState() right after this.
    for (int i = 0; i < JSFX_NUM_PARAMS && i < static_cast<int>(values.size()); ++i)
      GetParam(i)->Set(SliderToParam(i, values[i]));
  }
  return pos;
}

// =============================================================================
// UI
// =============================================================================
#if IPLUG_EDITOR
void JsfxPlugin::OnHostSelectedViewConfiguration(int width, int height)
{
#if JSFX_RESIZABLE
  if (GetUI())
    GetUI()->Resize(width, height, 1.f, true);
#else
  (void)width; (void)height;               // fixed-size editor: ignore
#endif
}

void JsfxPlugin::LayoutUI(IGraphics* pGraphics)
{
  const IRECT bounds = pGraphics->GetBounds();

  auto knobRect = [&](int visibleIdx) {
    const int row = visibleIdx / kKnobsPerRow, col = visibleIdx % kKnobsPerRow;
    return IRECT(kMargin + col * kCellW, kHeaderH + row * kCellH,
                 kMargin + (col + 1) * kCellW, kHeaderH + (row + 1) * kCellH).GetPadded(-6.f);
  };

  if (pGraphics->NControls())   // window resized: re-layout what exists
  {
    pGraphics->GetBackgroundControl()->SetTargetAndDrawRECTs(bounds);
    if (IControl* gfx = pGraphics->GetControlWithTag(kCtrlTagGfx))
      gfx->SetTargetAndDrawRECTs(bounds);
    if (IControl* status = pGraphics->GetControlWithTag(kCtrlTagStatus))
      status->SetTargetAndDrawRECTs(bounds);
    return;
  }

#if JSFX_RESIZABLE
  pGraphics->SetLayoutOnResize(true);
  pGraphics->AttachCornerResizer(EUIResizerMode::Size, true);
#endif
  pGraphics->EnableMouseOver(true);
  pGraphics->LoadFont("Roboto-Regular", const_cast<unsigned char*>(jsfx_generated::kFont.files[0].data),
                      static_cast<int>(jsfx_generated::kFont.files[0].size));
  pGraphics->AttachPanelBackground(kBgColor);

  if (mEngine->HasGfx())
  {
    // the JSFX draws everything itself
    pGraphics->AttachControl(new JsfxGfxControl(bounds, *mEngine), kCtrlTagGfx);
  }
  else
  {
    const IText title(22.f, kAccent, "Roboto-Regular", EAlign::Near);
    pGraphics->AttachControl(new ITextControl(IRECT(kMargin, 12.f, bounds.R - kMargin, 44.f), JSFX_DESC, title, kBgColor), kCtrlTagTitle);
    const IVStyle style = DEFAULT_STYLE
      .WithColor(kFG, IColor(255, 60, 60, 70))
      .WithColor(kPR, kAccent)
      .WithColor(kX1, kAccent)
      .WithLabelText(IText(13.f, COLOR_LIGHT_GRAY, "Roboto-Regular"))
      .WithValueText(IText(12.f, kAccent, "Roboto-Regular"))
      .WithDrawShadows(false);
    int visible = 0;
    for (int i = 0; i < JSFX_NUM_PARAMS; ++i)
    {
      if (!kJsfxParams[i].visible) continue;
      pGraphics->AttachControl(new IVKnobControl(knobRect(visible++), i, kJsfxParams[i].name, style, true));
    }
  }

  // compile errors / missing file, drawn on top of everything
  pGraphics->AttachControl(new JsfxStatusControl(bounds, *mEngine), kCtrlTagStatus);
}
#endif
