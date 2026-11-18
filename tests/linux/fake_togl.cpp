// Stand-in for libtogl.so: same C++ signatures (so the mangled names match the real exports), the engine side of
// the GL state machine, and ToGL's LAZY FBO binding: SetRenderTarget only remembers the target; the draw binds it.
#include "decls.h"
#include <string.h>
static IDirect3DSurface9 g_pool[60000]; static unsigned g_pool_next;     // short-lived GetSurfaceLevel surfaces: tens of thousands of distinct addresses, like real ToGL's heap objects (far more than the shim's cache has slots)
static IDirect3DSurface9 g_rtpool[64]; static unsigned g_rtpool_next;     // long-lived CreateRenderTarget surfaces
static IDirect3DTexture9 g_tex[512]; static unsigned g_tex_next;
static IDirect3DSurface9 *g_pending;
struct FboMap { unsigned type, name, fbo; } g_fbos[512]; static unsigned g_nfbos;
static unsigned fbo_for(unsigned type, unsigned name) {
  for (unsigned i = 0; i < g_nfbos; i++) if (g_fbos[i].type == type && g_fbos[i].name == name) return g_fbos[i].fbo;
  g_fbos[g_nfbos] = { type, name, mock_make_fbo((int)type, name) }; return g_fbos[g_nfbos++].fbo; }
extern "C" IDirect3D9* Direct3DCreate9(unsigned) { static IDirect3D9 d; return &d; }
long IDirect3D9::CreateDevice(unsigned, _D3DDEVTYPE, void*, unsigned, _D3DPRESENT_PARAMETERS_*, IDirect3DDevice9** pp) { static char dev; *pp = (IDirect3DDevice9*)&dev; return 0; }
long IDirect3DDevice9::Present(const _RECT*, const _RECT*, void*, const RGNDATA*) { return 0; }
long IDirect3DDevice9::SetRenderTarget(unsigned idx, IDirect3DSurface9* s) { if (idx == 0) g_pending = s; return 0; }
long IDirect3DDevice9::DrawPrimitive(_D3DPRIMITIVETYPE, unsigned, unsigned) { if (g_pending) mock_bind_draw(fbo_for(g_pending->gl_type, g_pending->gl_name)); return 0; }
long IDirect3DDevice9::StretchRect(IDirect3DSurface9*, const _RECT*, IDirect3DSurface9*, const _RECT*, _D3DTEXTUREFILTERTYPE) { return 0; }
long IDirect3DDevice9::Reset(_D3DPRESENT_PARAMETERS_*) { return 0; }
long IDirect3DDevice9::CreateTexture(unsigned w, unsigned h, unsigned, unsigned, _D3DFORMAT, _D3DPOOL, IDirect3DTexture9** pp, void**, char*) {
  IDirect3DTexture9 *t = &g_tex[g_tex_next++ % 512]; t->gl_name = mock_make_texture((int)w, (int)h); t->w = (int)w; t->h = (int)h; *pp = t; return 0; }
long IDirect3DDevice9::CreateRenderTarget(unsigned w, unsigned h, _D3DFORMAT, _D3DMULTISAMPLE_TYPE, unsigned, int, IDirect3DSurface9** pp, void**, char*) {
  IDirect3DSurface9 *s = &g_rtpool[g_rtpool_next++ % 64]; s->gl_type = 0x8D41; s->gl_name = mock_make_renderbuffer((int)w, (int)h, 0); s->w = (int)w; s->h = (int)h; *pp = s; return 0; }
// ToGL hands out a fresh surface object on (nearly) every call: model that, with pointer reuse after a while.
long IDirect3DTexture9::GetSurfaceLevel(unsigned, IDirect3DSurface9** pp) {
  IDirect3DSurface9 *s = &g_pool[g_pool_next++ % 60000]; s->gl_type = 0x1702; s->gl_name = gl_name; s->w = w; s->h = h; *pp = s; return 0; }
