/* Stand-in for libtogl.so: the "real" methods the shim forwards to. Returns fresh fake pointers. */
#include <stdint.h>
#include <stddef.h>
typedef int32_t HRESULT;
static unsigned long next_ptr = 0x10000;
static void *fresh(void) { next_ptr += 0x40; return (void *)next_ptr; }
HRESULT real_present(void *s, const void *a, const void *b, void *c, const void *d) __asm__("_ZN16IDirect3DDevice97PresentEPK5_RECTS2_PvPK7RGNDATA");
HRESULT real_present(void *s, const void *a, const void *b, void *c, const void *d) { (void)s;(void)a;(void)b;(void)c;(void)d; return 0; }
HRESULT real_setrt(void *s, unsigned i, void *surf) __asm__("_ZN16IDirect3DDevice915SetRenderTargetEjP17IDirect3DSurface9");
HRESULT real_setrt(void *s, unsigned i, void *surf) { (void)s;(void)i;(void)surf; return 0; }
HRESULT real_stretch(void *s, void *a, const void *b, void *c, const void *d, unsigned f) __asm__("_ZN16IDirect3DDevice911StretchRectEP17IDirect3DSurface9PK5_RECTS1_S4_21_D3DTEXTUREFILTERTYPE");
HRESULT real_stretch(void *s, void *a, const void *b, void *c, const void *d, unsigned f) { (void)s;(void)a;(void)b;(void)c;(void)d;(void)f; return 0; }
HRESULT real_ctex(void *s, unsigned w, unsigned h, unsigned l, unsigned u, int f, int p, void **pp, void **sh, char *lb) __asm__("_ZN16IDirect3DDevice913CreateTextureEjjjj10_D3DFORMAT8_D3DPOOLPP17IDirect3DTexture9PPvPc");
HRESULT real_ctex(void *s, unsigned w, unsigned h, unsigned l, unsigned u, int f, int p, void **pp, void **sh, char *lb) { (void)s;(void)w;(void)h;(void)l;(void)u;(void)f;(void)p;(void)sh;(void)lb; *pp = fresh(); return 0; }
HRESULT real_crt(void *s, unsigned w, unsigned h, int f, int ms, unsigned mq, int lk, void **pp, void **sh, char *lb) __asm__("_ZN16IDirect3DDevice918CreateRenderTargetEjj10_D3DFORMAT20_D3DMULTISAMPLE_TYPEjiPP17IDirect3DSurface9PPvPc");
HRESULT real_crt(void *s, unsigned w, unsigned h, int f, int ms, unsigned mq, int lk, void **pp, void **sh, char *lb) { (void)s;(void)w;(void)h;(void)f;(void)ms;(void)mq;(void)lk;(void)sh;(void)lb; *pp = fresh(); return 0; }
HRESULT real_reset(void *s, void *p) __asm__("_ZN16IDirect3DDevice95ResetEP23_D3DPRESENT_PARAMETERS_");
HRESULT real_reset(void *s, void *p) { (void)s;(void)p; return 0; }
HRESULT real_cdev(void *s, unsigned a, int d, void *h, unsigned b, void *p, void **pp) __asm__("_ZN10IDirect3D912CreateDeviceEj11_D3DDEVTYPEPvjP23_D3DPRESENT_PARAMETERS_PP16IDirect3DDevice9");
HRESULT real_cdev(void *s, unsigned a, int d, void *h, unsigned b, void *p, void **pp) { (void)s;(void)a;(void)d;(void)h;(void)b;(void)p; *pp = fresh(); return 0; }
HRESULT real_gsl(void *s, unsigned lvl, void **pp) __asm__("_ZN17IDirect3DTexture915GetSurfaceLevelEjPP17IDirect3DSurface9");
HRESULT real_gsl(void *s, unsigned lvl, void **pp) { (void)s;(void)lvl; *pp = fresh(); return 0; }
