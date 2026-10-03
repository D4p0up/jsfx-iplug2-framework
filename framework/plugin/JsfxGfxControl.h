// =============================================================================
//  JsfxGfxControl.h  -  IGraphics controls showing a JSFX
//
//  JsfxGfxControl    full-window view of the JSFX @gfx section. Input goes to
//                    the JSFX, @gfx runs on jsfx::GfxRunner's thread, finished
//                    frames are uploaded to a NanoVG texture.
//  JsfxStatusControl overlay showing load / compile errors (handy with
//                    JSFX_DEV_RELOAD: fix the file, save, the overlay goes away).
// =============================================================================
#pragma once

#include <memory>
#include <string>

#include "IControl.h"
#include "IGraphicsPopupMenu.h"
#include "JsfxEngine.h"
#include "JsfxGfxRunner.h"

#if !defined(IGRAPHICS_NANOVG)
  #error "JsfxGfxControl uploads @gfx frames with NanoVG: build with IGRAPHICS_BACKEND=NANOVG (iPlug2's default)"
#endif

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

class JsfxGfxControl : public IControl
{
public:
  JsfxGfxControl(const IRECT& bounds, jsfx::Engine& engine);
  ~JsfxGfxControl() override;

  void Draw(IGraphics& g) override;
  bool IsDirty() override;   // also the per-frame tick (called at PLUG_FPS)

  void OnMouseDown(float x, float y, const IMouseMod& mod) override;
  void OnMouseUp(float x, float y, const IMouseMod& mod) override;
  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override;
  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override { OnMouseDown(x, y, mod); }
  void OnMouseOver(float x, float y, const IMouseMod& mod) override;
  void OnMouseOut() override;
  void OnMouseWheel(float x, float y, const IMouseMod& mod, float d) override;
  bool OnKeyDown(float x, float y, const IKeyPress& key) override;
  bool OnKeyUp(float x, float y, const IKeyPress& key) override;
  void OnDropMultiple(const std::vector<const char*>& paths) override;
  void OnDrop(const char* str) override { OnDropMultiple({str}); }
  void OnPopupMenuSelection(IPopupMenu* pSelectedMenu, int valIdx) override;

private:
  void UpdateMouse(float x, float y, const IMouseMod& mod, bool pressed);
  bool PushKey(const IKeyPress& key, bool press);
  void ShowPendingMenu();

  std::unique_ptr<jsfx::GfxRunner> mRunner;
  jsfx::GfxInput mInput;                 // accumulated between frames
  uint64_t mFrameSerial = 0;
  double mLastRequest = 0.0;

  // NanoVG texture
  void* mVG = nullptr;
  int mImage = 0, mImageW = 0, mImageH = 0;

  std::unique_ptr<IPopupMenu> mMenu;
  bool mMenuOpen = false;
};

class JsfxStatusControl : public IControl
{
public:
  JsfxStatusControl(const IRECT& bounds, jsfx::Engine& engine);
  void Draw(IGraphics& g) override;
  bool IsDirty() override;
  bool IsHit(float, float) const override { return false; }   // never steals the mouse

private:
  jsfx::Engine& mEngine;
  std::string mText;
  int mTicks = 0;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
