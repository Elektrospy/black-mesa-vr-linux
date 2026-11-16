/* Test driver: fake SDL + fake GL (with LAZY framebuffer binding, like ToGL) + a fake "engine" frame loop.
 * Calls the D3D methods by their mangled names, so the LD_PRELOADed shim intercepts them. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
typedef int32_t HRESULT;

HRESULT d_present(void *s, const void *a, const void *b, void *c, const void *d) __asm__("_ZN16IDirect3DDevice97PresentEPK5_RECTS2_PvPK7RGNDATA");
HRESULT d_setrt(void *s, unsigned i, void *surf) __asm__("_ZN16IDirect3DDevice915SetRenderTargetEjP17IDirect3DSurface9");
HRESULT d_ctex(void *s, unsigned w, unsigned h, unsigned l, unsigned u, int f, int p, void **pp, void **sh, char *lb) __asm__("_ZN16IDirect3DDevice913CreateTextureEjjjj10_D3DFORMAT8_D3DPOOLPP17IDirect3DTexture9PPvPc");
HRESULT d_gsl(void *s, unsigned lvl, void **pp) __asm__("_ZN17IDirect3DTexture915GetSurfaceLevelEjPP17IDirect3DSurface9");
HRESULT d_crt(void *s, unsigned w, unsigned h, int f, int ms, unsigned mq, int lk, void **pp, void **sh, char *lb) __asm__("_ZN16IDirect3DDevice918CreateRenderTargetEjj10_D3DFORMAT20_D3DMULTISAMPLE_TYPEjiPP17IDirect3DSurface9PPvPc");

/* ---------------- fake GL ---------------- */
#define MAXO 2048
static int draw_fbo, read_fbo, tex_bind, rb_bind, scissor = 1, read_buffer = 0x8CE0, err;
static struct { int type; unsigned name; } fbo_att[MAXO];     /* colour attachment 0 per FBO */
static struct { int w, h, ms; } tex_info[MAXO], rb_info[MAXO];
static unsigned next_fbo = 100, next_tex = 1000;
static int finish_calls, ctx_current = 1, errors_from_shim;
struct blit { int src_fbo, src_type; unsigned src_name; int sx1, sy1, dst_fbo; unsigned dst_name; int dy0, dy1, dx1; } blits[64];
static int nblits;
static int sdl_ctx_dummy;

void *SDL_GL_GetCurrentContext(void) { return ctx_current ? &sdl_ctx_dummy : NULL; }
static const char *EXTS[] = { "GL_EXT_memory_object", "GL_EXT_memory_object_fd", "GL_EXT_semaphore", "GL_EXT_semaphore_fd" };
static const unsigned char *f_GetString(unsigned n) { return (const unsigned char *)(n == 0x1F00 ? "Fake" : n == 0x1F01 ? "FakeGL" : "4.6"); }
static const unsigned char *f_GetStringi(unsigned n, unsigned i) { (void)n; return i < 4 ? (const unsigned char *)EXTS[i] : NULL; }
static void f_GetIntegerv(unsigned p, int *v)
{
    switch (p) {
    case 0x8CA6: *v = draw_fbo; break;
    case 0x8CAA: *v = read_fbo; break;
    case 0x8069: *v = tex_bind; break;
    case 0x8CA7: *v = rb_bind; break;
    case 0x821D: *v = 4; break;
    case 0x0C02: *v = read_buffer; break;
    case 0x0BA2: v[0] = 0; v[1] = 0; v[2] = 1280; v[3] = 720; break;
    default: *v = 0;
    }
}
static unsigned f_GetError(void) { unsigned e = err; err = 0; return e; }
static void f_BindFramebuffer(unsigned t, unsigned f) { if (t == 0x8CA8 || t == 0x8D40) read_fbo = (int)f; if (t == 0x8CA9 || t == 0x8D40) draw_fbo = (int)f; }
static int *bound(unsigned t) { return t == 0x8CA8 ? &read_fbo : &draw_fbo; }
static void f_FramebufferTexture2D(unsigned t, unsigned a, unsigned tt, unsigned tex, int l) { (void)a;(void)tt;(void)l; int f = *bound(t); fbo_att[f].type = tex ? 0x1702 : 0; fbo_att[f].name = tex; }
static void f_FramebufferRenderbuffer(unsigned t, unsigned a, unsigned rt, unsigned rb) { (void)a;(void)rt; int f = *bound(t); fbo_att[f].type = rb ? 0x8D41 : 0; fbo_att[f].name = rb; }
static unsigned f_CheckFramebufferStatus(unsigned t) { int f = *bound(t); return fbo_att[f].name ? 0x8CD5 : 0x8CD7; }
static void f_GetFramebufferAttachmentParameteriv(unsigned t, unsigned a, unsigned p, int *v)
{
    (void)a; int f = *bound(t);
    if (!f) { err = 0x500; return; }                         /* default framebuffer: invalid enum, like real GL */
    *v = p == 0x8CD0 ? fbo_att[f].type : (int)fbo_att[f].name;
}
static void f_BindTexture(unsigned t, unsigned n) { (void)t; tex_bind = (int)n; }
static void f_GetTexLevelParameteriv(unsigned t, int l, unsigned p, int *v) { (void)t;(void)l; if (!tex_bind || tex_info[tex_bind].ms) { err = 0x502; return; } *v = p == 0x1000 ? tex_info[tex_bind].w : tex_info[tex_bind].h; }
static void f_BindRenderbuffer(unsigned t, unsigned n) { (void)t; rb_bind = (int)n; }
static void f_GetRenderbufferParameteriv(unsigned t, unsigned p, int *v) { (void)t; *v = p == 0x8D42 ? rb_info[rb_bind].w : p == 0x8D43 ? rb_info[rb_bind].h : rb_info[rb_bind].ms; }
static void f_BlitFramebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, unsigned m, unsigned f)
{
    (void)sx0;(void)sy0;(void)dx0;(void)m;(void)f;
    if (scissor) { errors_from_shim++; }                       /* the shim must disable scissor for blits */
    struct blit *b = &blits[nblits < 63 ? nblits++ : 63];
    b->src_fbo = read_fbo; b->src_type = fbo_att[read_fbo].type; b->src_name = fbo_att[read_fbo].name; b->sx1 = sx1; b->sy1 = sy1;
    b->dst_fbo = draw_fbo; b->dst_name = fbo_att[draw_fbo].name; b->dy0 = dy0; b->dy1 = dy1; b->dx1 = dx1;
}
static void f_GenFramebuffers(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = next_fbo++; }
static void f_GenTextures(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = next_tex++; }
static void f_nop(void) {}
static void f_Finish(void) { finish_calls++; }
static unsigned char f_IsEnabled(unsigned c) { return c == 0x0C11 ? (unsigned char)scissor : 0; }
static void f_Enable(unsigned c) { if (c == 0x0C11) scissor = 1; }
static void f_Disable(unsigned c) { if (c == 0x0C11) scissor = 0; }
static void f_CreateMemoryObjects(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = 7; }
static void f_ImportMemoryFd(unsigned m, uint64_t sz, unsigned ht, int fd) { (void)m;(void)sz;(void)ht; close(fd); }
static void f_TexStorageMem2D(unsigned t, int lv, unsigned fmt, int w, int h, unsigned mem, uint64_t off) { (void)t;(void)lv;(void)fmt;(void)mem;(void)off; tex_info[tex_bind].w = w; tex_info[tex_bind].h = h; }

static struct { const char *n; void *f; } TABLE[] = {
    {"glGetString", f_GetString}, {"glGetStringi", f_GetStringi}, {"glGetIntegerv", f_GetIntegerv}, {"glGetError", f_GetError},
    {"glBindFramebuffer", f_BindFramebuffer}, {"glFramebufferTexture2D", f_FramebufferTexture2D},
    {"glFramebufferRenderbuffer", f_FramebufferRenderbuffer}, {"glCheckFramebufferStatus", f_CheckFramebufferStatus},
    {"glGetFramebufferAttachmentParameteriv", f_GetFramebufferAttachmentParameteriv}, {"glBindTexture", f_BindTexture},
    {"glGetTexLevelParameteriv", f_GetTexLevelParameteriv}, {"glBindRenderbuffer", f_BindRenderbuffer},
    {"glGetRenderbufferParameteriv", f_GetRenderbufferParameteriv}, {"glBlitFramebuffer", f_BlitFramebuffer},
    {"glGenFramebuffers", f_GenFramebuffers}, {"glGenTextures", f_GenTextures}, {"glDeleteFramebuffers", f_nop},
    {"glDeleteTextures", f_nop}, {"glTexParameteri", f_nop}, {"glFinish", f_Finish}, {"glIsEnabled", f_IsEnabled},
    {"glEnable", f_Enable}, {"glDisable", f_Disable}, {"glCreateMemoryObjectsEXT", f_CreateMemoryObjects},
    {"glDeleteMemoryObjectsEXT", f_nop}, {"glMemoryObjectParameterivEXT", f_nop}, {"glImportMemoryFdEXT", f_ImportMemoryFd},
    {"glTexStorageMem2DEXT", f_TexStorageMem2D}, {"glGetUnsignedBytevEXT", f_nop},
};
void *SDL_GL_GetProcAddress(const char *name)
{
    for (unsigned i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) if (!strcmp(TABLE[i].n, name)) return TABLE[i].f;
    return NULL;
}

/* ---------------- fake engine ---------------- */
static void *dev = (void *)0x1000, *S1, *S2, *S3, *BACK = (void *)0x2000;
static void draw_into(void *surf)   /* ToGL flushes the pending framebuffer binding at the first draw */
{
    draw_fbo = surf == S1 ? 5 : surf == S2 ? 6 : surf == S3 ? 7 : 0;
}
static void setrt(void *s) { d_setrt(dev, 0, s); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s  ", __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "normal";
    int frames = argc > 2 ? atoi(argv[2]) : 800;
    /* engine objects: three render targets (a big one, a small one, an MSAA one) + the back buffer */
    fbo_att[5].type = 0x1702; fbo_att[5].name = 77; tex_info[77].w = 3840; tex_info[77].h = 2160;
    fbo_att[6].type = 0x1702; fbo_att[6].name = 78; tex_info[78].w = 128;  tex_info[78].h = 128;
    fbo_att[7].type = 0x8D41; fbo_att[7].name = 9;  rb_info[9].w = 1280;  rb_info[9].h = 720; rb_info[9].ms = 4;
    void *t1, *t2, *t3;
    d_ctex(dev, 3840, 2160, 1, 1, 22, 0, &t1, NULL, "_rt_FullFrameFB");
    d_ctex(dev, 128, 128, 1, 1, 22, 0, &t2, NULL, "_rt_SmallHDR0");
    d_crt(dev, 1280, 720, 22, 4, 0, 0, &S3, NULL, "msaa_color");
    if (strcmp(mode, "nosurf")) { d_gsl(t1, 0, &S1); d_gsl(t2, 0, &S2); }
    else { S1 = (void *)0x3000; S2 = (void *)0x3100; }       /* GetSurfaceLevel not exported: surfaces unknown to the registry */
    (void)t3;

    int bad_state = 0;
    for (int f = 0; f < frames; f++) {
        if (!strcmp(mode, "nocontext")) ctx_current = (f < 2);  /* context only for the very first Presents */
        if (!strcmp(mode, "lazy")) {
            /* S1 is bound but NOTHING is drawn before the next SetRenderTarget: GL still holds S2's FBO */
            setrt(S2); draw_into(S2);
            setrt(S1);                       /* no draw */
            setrt(BACK); draw_into(BACK);
        } else {
            setrt(S1); draw_into(S1);
            setrt(S2); draw_into(S2);
            setrt(S3); draw_into(S3);
            setrt(S1); draw_into(S1);        /* S1 is left again: this is its final content of the frame */
            setrt(BACK); draw_into(BACK);
        }
        int keep_draw = draw_fbo, keep_read = read_fbo = 4242, keep_tex = tex_bind = 555, keep_rb = rb_bind = 666;
        d_present(dev, NULL, NULL, NULL, NULL);
        if (draw_fbo != keep_draw || read_fbo != keep_read || tex_bind != keep_tex || rb_bind != keep_rb || scissor != 1 || err) bad_state++;
        read_fbo = 0; tex_bind = 0; rb_bind = 0;
    }
    CHECK(bad_state == 0, "GL state not restored in %d frames (draw/read fbo, tex/rb binding, scissor, error)", bad_state);
    CHECK(errors_from_shim == 0, "blit ran with scissor enabled %d times", errors_from_shim);
    printf("DRIVER blits=%d finish=%d\n", nblits, finish_calls);
    for (int i = 0; i < nblits && i < 8; i++)
        printf("BLIT %d: read fbo %d (att %s %u) %dx%d -> draw fbo %d (att %u) dst y %d..%d x1=%d\n", i, blits[i].src_fbo,
               blits[i].src_type == 0x1702 ? "tex" : blits[i].src_type == 0x8D41 ? "rb" : "none", blits[i].src_name,
               blits[i].sx1, blits[i].sy1, blits[i].dst_fbo, blits[i].dst_name, blits[i].dy0, blits[i].dy1, blits[i].dx1);
    if (!strcmp(mode, "normal") && !getenv("BMVR_TARGET")) {
        CHECK(nblits >= 3, "expected >=3 snapshots, got %d", nblits);
        for (int i = 0; i < nblits; i++) {
            CHECK(blits[i].src_name == 77 && blits[i].src_type == 0x1702, "blit %d source is %u not the FullFrameFB texture 77", i, blits[i].src_name);
            CHECK(blits[i].sx1 == 3840 && blits[i].sy1 == 2160, "blit %d size %dx%d", i, blits[i].sx1, blits[i].sy1);
            CHECK(blits[i].dst_name == 1000, "blit %d dst att %u (expected imported texture 1000)", i, blits[i].dst_name);
            int flipped = blits[i].dy0 > blits[i].dy1;
            CHECK(flipped == (i & 1), "blit %d flip alternation (flipped=%d)", i, flipped);
        }
    }
    return fails ? 1 : 0;
}
