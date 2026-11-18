// Stand-in for the engine: some "loading" frames (only Present), then "gameplay" frames with many SetRenderTarget(0)
// switches ending on the back buffer, like the real game (~400 per frame; fewer here).
#include "decls.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
  int loading = argc > 1 ? atoi(argv[1]) : 300, play = argc > 2 ? atoi(argv[2]) : 500, switches = 60;
  IDirect3D9 *d3d = Direct3DCreate9(32); IDirect3DDevice9 *dev = 0; _D3DPRESENT_PARAMETERS_ pp{1};
  d3d->CreateDevice(0, D0, 0, 0x40, &pp, &dev);
  IDirect3DSurface9 *back = 0; dev->CreateRenderTarget(3840, 2160, F0, M0, 0, 0, &back, 0, (char*)"InternalRT0");
  struct { const char *n; int w, h; } rts[] = { {"_rt_fullframefb",3840,2160}, {"_rt_hud",3840,2160}, {"_rt_gbnormal",3840,2160}, {"_rt_smallfb0",960,540}, {"_rt_csmshadowbuffer",4096,4096} };
  IDirect3DTexture9 *tx[5]; for (int i = 0; i < 5; i++) dev->CreateTexture((unsigned)rts[i].w, (unsigned)rts[i].h, 1, 1, F0, P0, &tx[i], 0, (char*)rts[i].n);
  int violations = 0;
  for (int f = 0; f < loading + play; f++) {
    if (f >= loading) {
      for (int k = 0; k < switches; k++) {
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
  printf("GAME done: frames=%d gl_state_violations=%d\n", loading + play, violations);
  return violations ? 1 : 0;
}
