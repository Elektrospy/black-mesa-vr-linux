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
static unsigned char g_main_raw[0x148];   // the engine builds nested views (water) from its own state, not from the view it was handed
static IDirect3DDevice9 *g_dev; static IDirect3DSurface9 *g_back; static IDirect3DTexture9 *g_tx[5]; static int g_switches = 60, g_nest;
struct CViewRender : IViewRender {
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
__attribute__((noinline)) static IViewRender *make_view_render() { return new CViewRender(); }   // opaque: the compiler must emit the vtable and call through it
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
    unsigned dr = mock_draw();               // the lazily bound FBO must stay as ToGL left it (the shim reads it to learn the target)
    mock_set_state(dr, 8, 9, 10, 1);         // everything else the engine has bound must survive the shim's work inside Present
    dev->Present(0, 0, 0, 0);
    if (!mock_state_ok(dr, 8, 9, 10, 1)) violations++;
    if (f % 4 == 0) usleep(200);
  }
  printf("GAME done: frames=%d gl_state_violations=%d\n", loading + play + tail, violations);
  return violations ? 1 : 0;
}
