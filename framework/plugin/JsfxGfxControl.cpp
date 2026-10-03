#include "JsfxGfxControl.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#include "IGraphics.h"
#include "nanovg.h"

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

namespace {

double NowSeconds()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint32_t Mods(const IMouseMod& mod)
{
  uint32_t m = 0;
  if (mod.S) m |= ysfx_mod_shift;
  if (mod.A) m |= ysfx_mod_alt;
#if defined(OS_MAC)
  if (mod.C) m |= ysfx_mod_super;   // iPlug2 reports Cmd as C on macOS
#else
  if (mod.C) m |= ysfx_mod_ctrl;
#endif
  return m;
}

uint32_t Mods(const IKeyPress& key)
{
  return Mods(IMouseMod(false, false, key.S, key.C, key.A));
}

uint32_t DecodeUtf8(const char* s)
{
  const auto* u = reinterpret_cast<const unsigned char*>(s);
  if (!u[0]) return 0;
  if (u[0] < 0x80) return u[0];
  if ((u[0] & 0xE0) == 0xC0 && u[1]) return ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu);
  if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) return ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
  if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3])
    return ((u[0] & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12) | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu);
  return 0;
}

// iPlug2 virtual key -> ysfx key code (0 = use the character)
uint32_t TranslateVK(int vk)
{
  switch (vk)
  {
    case kVK_BACK: return ysfx_key_backspace;
    case kVK_TAB: return '\t';
    case kVK_RETURN: return '\r';
    case kVK_ESCAPE: return ysfx_key_escape;
    case kVK_DELETE: return ysfx_key_delete;
    case kVK_LEFT: return ysfx_key_left;
    case kVK_UP: return ysfx_key_up;
    case kVK_RIGHT: return ysfx_key_right;
    case kVK_DOWN: return ysfx_key_down;
    case kVK_PRIOR: return ysfx_key_page_up;
    case kVK_NEXT: return ysfx_key_page_down;
    case kVK_HOME: return ysfx_key_home;
    case kVK_END: return ysfx_key_end;
    case kVK_INSERT: return ysfx_key_insert;
    case kVK_SPACE: return ' ';
    default: break;
  }
  if (vk >= kVK_F1 && vk <= kVK_F12)
    return ysfx_key_f1 + static_cast<uint32_t>(vk - kVK_F1);
  return 0;
}

// gfx_setcursor() ids are Windows OCR_* resource ids
ECursor TranslateCursor(int id)
{
  switch (id)
  {
    case 32513: return ECursor::IBEAM;
    case 32514: return ECursor::WAIT;
    case 32515: return ECursor::CROSS;
    case 32516: return ECursor::UPARROW;
    case 32642: return ECursor::SIZENWSE;
    case 32643: return ECursor::SIZENESW;
    case 32644: return ECursor::SIZEWE;
    case 32645: return ECursor::SIZENS;
    case 32646: return ECursor::SIZEALL;
    case 32648: return ECursor::INO;
    case 32649: return ECursor::HAND;
    case 32650: return ECursor::APPSTARTING;
    case 32651: return ECursor::HELP;
    default: return ECursor::ARROW;
  }
}

} // namespace

// =============================================================================
JsfxGfxControl::JsfxGfxControl(const IRECT& bounds, jsfx::Engine& engine)
: IControl(bounds)
, mRunner(std::make_unique<jsfx::GfxRunner>(engine))
{
  mIgnoreMouse = false;
  mInput.visible = true;
}

JsfxGfxControl::~JsfxGfxControl()
{
  // Stops the @gfx thread (and releases a pending gfx_showmenu) first.
  mRunner.reset();
  // The NanoVG context is owned by IGraphics and may already be gone: the
  // texture dies with it, so it is deliberately not deleted here.
}

// ------------------------------------------------------------------ frame tick
bool JsfxGfxControl::IsDirty()
{
  if (!mRunner)
    return IControl::IsDirty();

  if (!mMenuOpen)
    ShowPendingMenu();

  const int cursor = mRunner->TakeCursorRequest();
  if (cursor >= 0 && GetUI())
    GetUI()->SetMouseCursor(TranslateCursor(cursor));

  const double now = NowSeconds();
  const double interval = 1.0 / std::max<uint32_t>(1, mRunner->RequestedFps());
  if (!mRunner->IsBusy() && now - mLastRequest >= interval * 0.95)
  {
    mLastRequest = now;
    const float scale = GetUI() ? GetUI()->GetTotalScale() : 1.f;   // screen x draw scale
    jsfx::GfxInput in = mInput;
    mInput.wheel = mInput.hwheel = 0.0;
    mInput.keys.clear();
    mRunner->RequestFrame(static_cast<int>(mRECT.W()), static_cast<int>(mRECT.H()), scale, std::move(in));
  }

  // a finished frame is waiting to be uploaded?
  return mRunner->LatestFrame() != mFrameSerial || IControl::IsDirty();
}

void JsfxGfxControl::Draw(IGraphics& g)
{
  auto* vg = static_cast<NVGcontext*>(g.GetDrawContext());
  if (!vg || !mRunner)
    return;

  if (vg != mVG)   // new graphics context (editor reopened): old texture is gone
  {
    mVG = vg;
    mImage = 0;
    mImageW = mImageH = 0;
    mFrameSerial = 0;
  }

  mRunner->ConsumeFrame(mFrameSerial, [&](const uint8_t* rgba, int w, int h) {
    if (mImage && (w != mImageW || h != mImageH))
    {
      nvgDeleteImage(vg, mImage);
      mImage = 0;
    }
    if (!mImage)
    {
      mImage = nvgCreateImageRGBA(vg, w, h, 0, rgba);
      mImageW = w;
      mImageH = h;
    }
    else
    {
      nvgUpdateImage(vg, mImage, rgba);
    }
  });

  if (!mImage)
  {
    g.FillRect(IColor(255, 28, 28, 34), mRECT);
    return;
  }

  // The frame is (W x H) logical units, rendered at 1x or at the backing scale
  // when the JSFX sets gfx_ext_retina: either way it maps onto mRECT.
  const NVGpaint paint = nvgImagePattern(vg, mRECT.L, mRECT.T, mRECT.W(), mRECT.H(), 0.f, mImage, 1.f);
  nvgBeginPath(vg);
  nvgRect(vg, mRECT.L, mRECT.T, mRECT.W(), mRECT.H());
  nvgFillPaint(vg, paint);
  nvgFill(vg);
}

// ------------------------------------------------------------------ input
void JsfxGfxControl::UpdateMouse(float x, float y, const IMouseMod& mod, bool)
{
  mInput.mouseX = x - mRECT.L;
  mInput.mouseY = y - mRECT.T;
  mInput.mods = Mods(mod);
  mInput.mouseOver = true;
}

void JsfxGfxControl::OnMouseDown(float x, float y, const IMouseMod& mod)
{
  UpdateMouse(x, y, mod, true);
  if (mod.L) mInput.buttons |= ysfx_button_left;
  if (mod.R) mInput.buttons |= ysfx_button_right;
  mInput.hasFocus = true;
}

void JsfxGfxControl::OnMouseUp(float x, float y, const IMouseMod& mod)
{
  UpdateMouse(x, y, mod, false);
  if (mod.L) mInput.buttons &= ~static_cast<uint32_t>(ysfx_button_left);
  if (mod.R) mInput.buttons &= ~static_cast<uint32_t>(ysfx_button_right);
  if (!mod.L && !mod.R) mInput.buttons = 0;
}

void JsfxGfxControl::OnMouseDrag(float x, float y, float, float, const IMouseMod& mod)
{
  UpdateMouse(x, y, mod, true);
}

void JsfxGfxControl::OnMouseOver(float x, float y, const IMouseMod& mod)
{
  UpdateMouse(x, y, mod, false);
  IControl::OnMouseOver(x, y, mod);
}

void JsfxGfxControl::OnMouseOut()
{
  mInput.mouseOver = false;
  IControl::OnMouseOut();
}

void JsfxGfxControl::OnMouseWheel(float x, float y, const IMouseMod& mod, float d)
{
  UpdateMouse(x, y, mod, false);
  mInput.wheel += d;
}

bool JsfxGfxControl::PushKey(const IKeyPress& key, bool press)
{
  uint32_t code = TranslateVK(key.VK);
  if (!code)
  {
    code = DecodeUtf8(key.utf8);
    if (!code && key.VK >= kVK_A && key.VK <= kVK_Z)
      code = static_cast<uint32_t>('a' + (key.VK - kVK_A));
    else if (!code && key.VK >= kVK_0 && key.VK <= kVK_9)
      code = static_cast<uint32_t>('0' + (key.VK - kVK_0));
    if (key.C && code >= 'A' && code <= 'Z')
      code += 'a' - 'A';
  }
  if (!code)
    return false;
  mInput.keys.push_back({Mods(key), code, press});
  return true;
}

bool JsfxGfxControl::OnKeyDown(float, float, const IKeyPress& key) { return PushKey(key, true); }
bool JsfxGfxControl::OnKeyUp(float, float, const IKeyPress& key) { return PushKey(key, false); }

void JsfxGfxControl::OnDropMultiple(const std::vector<const char*>& paths)
{
  std::vector<std::string> files;
  for (const char* p : paths)
    if (p) files.emplace_back(p);
  mRunner->SetDroppedFiles(std::move(files));
}

// ------------------------------------------------------------------ menus
void JsfxGfxControl::ShowPendingMenu()
{
  jsfx::GfxMenuRequest req;
  if (!mRunner->TakeMenuRequest(req) || !GetUI())
    return;

  ysfx_menu_t* desc = ysfx_parse_menu(req.spec.c_str());
  mMenu = std::make_unique<IPopupMenu>();
  if (desc)
  {
    std::vector<IPopupMenu*> chain{mMenu.get()};
    for (uint32_t i = 0; i < desc->insn_count; ++i)
    {
      const ysfx_menu_insn_t& insn = desc->insns[i];
      int flags = IPopupMenu::Item::kNoFlags;
      if (insn.item_flags & ysfx_menu_item_disabled) flags |= IPopupMenu::Item::kDisabled;
      if (insn.item_flags & ysfx_menu_item_checked) flags |= IPopupMenu::Item::kChecked;
      switch (insn.opcode)
      {
        case ysfx_menu_item:
          chain.back()->AddItem(new IPopupMenu::Item(insn.name ? insn.name : "", flags, static_cast<int>(insn.id)));
          break;
        case ysfx_menu_separator:
          chain.back()->AddSeparator();
          break;
        case ysfx_menu_sub:
          chain.push_back(new IPopupMenu());          // owned by its parent item once closed
          break;
        case ysfx_menu_endsub:
          if (chain.size() > 1)
          {
            IPopupMenu* sub = chain.back();
            chain.pop_back();
            chain.back()->AddItem(insn.name ? insn.name : "", sub);
          }
          break;
      }
    }
    while (chain.size() > 1)                           // unbalanced spec: close open submenus
    {
      IPopupMenu* sub = chain.back();
      chain.pop_back();
      chain.back()->AddItem("", sub);
    }
    ysfx_menu_free(desc);
  }

  mMenuOpen = true;
  GetUI()->CreatePopupMenu(*this, *mMenu, mRECT.L + req.x, mRECT.T + req.y);
}

void JsfxGfxControl::OnPopupMenuSelection(IPopupMenu* pSelectedMenu, int)
{
  mMenuOpen = false;
  int id = 0;
  if (pSelectedMenu)
    if (IPopupMenu::Item* item = pSelectedMenu->GetChosenItem())
      id = std::max(0, item->GetTag());
  // the menu was opened by a mouse press the JSFX already consumed
  mInput.buttons = 0;
  if (mRunner)
    mRunner->AnswerMenu(id);
}

// =============================================================================
JsfxStatusControl::JsfxStatusControl(const IRECT& bounds, jsfx::Engine& engine)
: IControl(bounds)
, mEngine(engine)
{
  mIgnoreMouse = true;
}

bool JsfxStatusControl::IsDirty()
{
  if (++mTicks % 15 == 0)   // a few times per second is plenty
  {
    std::string text = mEngine.LastError();
    if (text.empty() && !mEngine.IsLoaded())
      text = "JSFX not loaded";
    if (text != mText)
    {
      mText = text;
      SetDirty(false);
    }
  }
  return IControl::IsDirty();
}

void JsfxStatusControl::Draw(IGraphics& g)
{
  if (mText.empty())
    return;
  std::vector<std::string> lines;
  std::istringstream ss(mText);
  for (std::string line; std::getline(ss, line) && lines.size() < 8;)
    lines.push_back(line);

  const float lineH = 16.f;
  const IRECT box = mRECT.GetFromTop(16.f + lineH * static_cast<float>(lines.size())).GetPadded(-6.f);
  g.FillRoundRect(IColor(235, 120, 24, 24), box, 4.f);
  const IText text(13.f, COLOR_WHITE, "Roboto-Regular", EAlign::Near, EVAlign::Top);
  for (size_t i = 0; i < lines.size(); ++i)
  {
    const IRECT r(box.L + 8.f, box.T + 6.f + lineH * static_cast<float>(i), box.R - 8.f, box.T + 6.f + lineH * static_cast<float>(i + 1));
    g.DrawText(text, lines[i].c_str(), r);
  }
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
