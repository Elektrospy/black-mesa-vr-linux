// Stand-in for the engine: some "loading" frames (only Present), then "gameplay" frames with many SetRenderTarget(0)
// switches ending on the back buffer, like the real game (~400 per frame; fewer here).
#include "decls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
// FAKE_STEREO=1: the gameplay frame is drawn by a CViewRender::RenderView (vtable slot 6, like IViewRender on the Windows build) that takes
// a CViewSetup-shaped 0x148-byte struct, draws many targets, and makes one NESTED view (water) per call. Each call prints what it was given.
struct CVS { unsigned char raw[0x148]; };
static void vsf(CVS &v, unsigned o, float f) { memcpy(v.raw + o, &f, 4); }
static float vgf(const CVS &v, unsigned o) { float f; memcpy(&f, v.raw + o, 4); return f; }
static void vsi(CVS &v, unsigned o, int i) { memcpy(v.raw + o, &i, 4); }
struct IViewRender { virtual void Init() {} virtual void LevelInit() {} virtual void LevelShutdown() {} virtual void Shutdown() {} virtual void OnRenderStart() {} virtual void Render() {}
                     virtual void RenderView(const CVS &v, int clear, int what) = 0; virtual void Tail() {} };
// FAKE_INPUT=1: every gameplay frame the "engine" fills a CUserCmd from the keyboard (here: one button bit) and calls IClientMode::CreateMove through the
// vtable, like CInput does. ClientModeShared has the layout of the Linux build: a virtual destructor (two slots) and 20 virtuals before CreateMove = slot 22.
// FAKE_INPUT_BADCMD=1 passes a pointer to junk instead of a CUserCmd (a wrong slot would see exactly that).
struct FUserCmd { virtual ~FUserCmd() {} int command_number = 0, tick_count = 0; float viewangles[3] = {0,0,0}; float forwardmove = 0, sidemove = 0, upmove = 0; int buttons = 0;
                  unsigned char impulse = 0; int weaponselect = 0, weaponsubtype = 0, random_seed = 0; short mousedx = 0, mousedy = 0; bool hasbeenpredicted = false; };
struct IClientMode { virtual ~IClientMode() {}
  virtual void f0() {} virtual void f1() {} virtual void f2() {} virtual void f3() {} virtual void f4() {} virtual void f5() {} virtual void f6() {} virtual void f7() {} virtual void f8() {} virtual void f9() {}
  virtual void f10() {} virtual void f11() {} virtual void f12() {} virtual void f13() {} virtual void f14() {} virtual void f15() {} virtual void f16() {} virtual void f17() {} virtual void f18() {} virtual void f19() {}
  virtual bool CreateMove(float flInputSampleTime, FUserCmd *cmd) = 0; };
static int g_cm_calls;
struct ClientModeShared : IClientMode { bool CreateMove(float, FUserCmd *c) override { g_cm_calls++; c->buttons |= 0x10000; c->tick_count = c->command_number; return true; } };
// FAKE_INPUT_DERIVED=1: the engine instantiates a subclass (own vtable copy, inherits CreateMove), as the real game does: a hook on the base vtable alone is never called.
struct ClientModeBMSNormal : ClientModeShared { void f3() override {} };
struct ClientModeOwn : ClientModeShared { bool CreateMove(float t, FUserCmd *c) override { return ClientModeShared::CreateMove(t, c); } };   // overrides and calls the base directly
__attribute__((noinline)) static IClientMode *make_client_mode() { const char *d = getenv("FAKE_INPUT_DERIVED"); if (d && d[0] == '2') return new ClientModeOwn(); if (d) return new ClientModeBMSNormal(); return new ClientModeShared(); }
static unsigned char g_main_raw[0x148];   // the engine builds nested views (water) from its own state, not from the view it was handed
static IDirect3DDevice9 *g_dev; static IDirect3DSurface9 *g_back; static IDirect3DTexture9 *g_tx[5]; static int g_switches = 60, g_nest;
#ifndef VR_CLASS_NAME
#define VR_CLASS_NAME CViewRender   // -DVR_CLASS_NAME=CBMViewRender builds a game whose view class has another name (T31)
#endif
struct VR_CLASS_NAME : IViewRender {
  void RenderView(const CVS &v, int clear, int what) override {
    int n = g_nest++;
    printf("RV nest=%d w=%d h=%d fov=%.3f asp=%.4f offc=%d l=%.4f r=%.4f b=%.4f t=%.4f o=%.3f,%.3f,%.3f a=%.3f,%.3f,%.3f what=%d clear=%d\n", n, *(int*)(v.raw+0x10), *(int*)(v.raw+0x18),
           vgf(v,0xB4), vgf(v,0xE8), v.raw[0xEC], vgf(v,0xF8), vgf(v,0xFC), vgf(v,0xF4), vgf(v,0xF0), vgf(v,0xBC), vgf(v,0xC0), vgf(v,0xC4), vgf(v,0xC8), vgf(v,0xCC), vgf(v,0xD0), what, clear);
    if (n == 0) {   // the water view inside the main view: small, must never be doubled
      CVS w; memcpy(w.raw, g_main_raw, sizeof w.raw); vsi(w, 0x10, 1024); vsi(w, 0x18, 1024); RenderView(w, 0, 0);
    }
    if (n == 0 || true) {
      for (int k = 0; k < g_switches; k++) { IDirect3DSurface9 *s = 0; g_tx[k % 5]->GetSurfaceLevel(0, &s); g_dev->SetRenderTarget(0, s); g_dev->DrawPrimitive(PT0, 0, 1); }
      g_dev->SetRenderTarget(0, g_back); g_dev->DrawPrimitive(PT0, 0, 1);
    }
    g_nest--;
  }
};
__attribute__((noinline)) static IViewRender *make_view_render() { return new VR_CLASS_NAME(); }   // opaque: the compiler must emit the vtable and call through it
int main(int argc, char **argv) {
  int loading = argc > 1 ? atoi(argv[1]) : 300, play = argc > 2 ? atoi(argv[2]) : 500, tail = argc > 3 ? atoi(argv[3]) : 0, switches = 60;
  IDirect3D9 *d3d = Direct3DCreate9(32); IDirect3DDevice9 *dev = 0; _D3DPRESENT_PARAMETERS_ pp{1};
  d3d->CreateDevice(0, D0, 0, 0x40, &pp, &dev);
  IDirect3DSurface9 *back = 0; dev->CreateRenderTarget(3840, 2160, F0, M0, 0, 0, &back, 0, (char*)"InternalRT0");
  struct { const char *n; int w, h; } rts[] = { {"_rt_fullframefb",3840,2160}, {"_rt_hud",3840,2160}, {"_rt_gbnormal",3840,2160}, {"_rt_smallfb0",960,540}, {"_rt_csmshadowbuffer",4096,4096} };
  IDirect3DTexture9 *tx[5]; for (int i = 0; i < 5; i++) dev->CreateTexture((unsigned)rts[i].w, (unsigned)rts[i].h, 1, 1, F0, P0, &tx[i], 0, (char*)rts[i].n);
  // FAKE_SKIP_RT=<label> FAKE_SKIP_AFTER=<frame>: from that frame on the engine no longer renders that target (tests that a pair is never half-delivered)
  const char *skip_rt = getenv("FAKE_SKIP_RT"); int skip_after = getenv("FAKE_SKIP_AFTER") ? atoi(getenv("FAKE_SKIP_AFTER")) : 0, skip_idx = -1;
  for (int i = 0; i < 5; i++) if (skip_rt && !strcmp(skip_rt, rts[i].n)) skip_idx = i;
  int violations = 0; g_dev = dev; g_back = back; for (int i = 0; i < 5; i++) g_tx[i] = tx[i];
  IViewRender *vr = make_view_render(); const bool stereo = getenv("FAKE_STEREO") != 0;
  const bool input = getenv("FAKE_INPUT") != 0; IClientMode *cmode = make_client_mode(); FUserCmd fu; unsigned char junk[256]; memset(junk, 0xAB, sizeof junk);
  int cm_n = 0, cm_move = 0, cm_attack = 0, cm_kbd_lost = 0, cm_imp100 = 0, cm_turned = 0, cm_after_bad = 0; float cm_maxfwd = 0, cm_minyaw = 0;
  CVS main_view; memset(&main_view, 0, sizeof main_view);
  if (getenv("FAKE_BAD_VIEW")) vsf(main_view,0xB4,5); // FAKE_BAD_VIEW: not a CViewSetup-looking view
  vsi(main_view,0x10,3840); vsi(main_view,0x18,2160); if (!getenv("FAKE_BAD_VIEW")) vsf(main_view,0xB4,90); vsf(main_view,0xE8,16.0f/9); vsf(main_view,0xD4,7); vsf(main_view,0xD8,30000);
  vsf(main_view,0xBC,100); vsf(main_view,0xC0,200); vsf(main_view,0xC4,50); vsf(main_view,0xCC,45);
  memcpy(g_main_raw, main_view.raw, sizeof g_main_raw);
  for (int f = 0; f < loading + play + tail; f++) {
    if (f >= loading && f < loading + play && stereo) { vr->RenderView(main_view, 7, 3); }
    else if (f >= loading && f < loading + play) {   // the tail is a second loading screen: Present only
      for (int k = 0; k < switches; k++) {
        if (skip_idx >= 0 && f >= skip_after && k % 5 == skip_idx) continue;
        IDirect3DSurface9 *s = 0; tx[k % 5]->GetSurfaceLevel(0, &s);
        dev->SetRenderTarget(0, s); dev->DrawPrimitive(PT0, 0, 1);
      }
      dev->SetRenderTarget(0, back); dev->DrawPrimitive(PT0, 0, 1);
    }
    if (input && f >= loading && f < loading + play) {
      fu = FUserCmd(); fu.command_number = ++cm_n; fu.viewangles[1] = 10.0f;
      if (getenv("FAKE_INPUT_BADCMD")) { cmode->CreateMove(0.011f, (FUserCmd *)junk); }
      else {
        cmode->CreateMove(0.011f, &fu);
        if (fu.forwardmove > 1.0f) cm_move++; if (fu.forwardmove > cm_maxfwd) cm_maxfwd = fu.forwardmove; if (fu.buttons & 1) cm_attack++;
        if (!(fu.buttons & 0x10000)) cm_kbd_lost++; if (fu.impulse == 100) cm_imp100++; if (fu.viewangles[1] < 10.0f) cm_turned++; if (fu.viewangles[1] < cm_minyaw || cm_minyaw == 0) cm_minyaw = fu.viewangles[1];
      }
    }
    unsigned dr = mock_draw();               // the lazily bound FBO must stay as ToGL left it (the shim reads it to learn the target)
    mock_set_state(dr, 8, 9, 10, 1);         // everything else the engine has bound must survive the shim's work inside Present
    dev->Present(0, 0, 0, 0);
    if (!mock_state_ok(dr, 8, 9, 10, 1)) violations++;
    if (f % 4 == 0) usleep(200);
  }
  if (input) printf("CM calls=%d cmds=%d moved=%d maxfwd=%.1f attack=%d kbd_lost=%d impulse100=%d turned=%d minyaw=%.2f\n", g_cm_calls, cm_n, cm_move, cm_maxfwd, cm_attack, cm_kbd_lost, cm_imp100, cm_turned, cm_minyaw);
  printf("GAME done: frames=%d gl_state_violations=%d\n", loading + play + tail, violations);
  return violations ? 1 : 0;
}
