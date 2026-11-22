/* Mock "libSDL2-2.0.so.0": SDL_GL_GetProcAddress + a small GL state machine that records every call it
 * receives into $MOCK_TRACE (one line per call, O_APPEND so it interleaves with the mock helper). */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

static int tfd = -2;
static void T(const char *fmt, ...)
{
    if (tfd == -2) { const char *p = getenv("MOCK_TRACE"); tfd = open(p ? p : "mock_trace.txt", O_WRONLY | O_CREAT | O_APPEND, 0644); }
    char b[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    b[n++] = '\n'; if (tfd >= 0 && write(tfd, b, (size_t)n) < 0) {}
}

#define MAXO 8192
struct fbo { int color_type; unsigned color_name; };
static struct { int w, h, samples; } tex[MAXO], rb[MAXO];
static struct fbo fb[MAXO];
static unsigned next_id = 1, bound_draw, bound_read, bound_tex, bound_rb, err;
static int scissor;
static unsigned sem_next = 1;

#define NOSEM (getenv("MOCK_NO_SEM") != NULL)

static const unsigned char *g_getString(unsigned n) { return (const unsigned char *)(n == 0x1F00 ? "MockVendor" : n == 0x1F01 ? "MockGPU" : "4.6 Mock"); }
static const char *exts[] = { "GL_EXT_memory_object", "GL_EXT_memory_object_fd", "GL_EXT_semaphore", "GL_EXT_semaphore_fd" };
static const unsigned char *g_getStringi(unsigned n, unsigned i) { (void)n; return (const unsigned char *)exts[i & 3]; }
static void g_getIntegerv(unsigned p, int *v)
{
    switch (p) {
    case 0x821D: v[0] = 4; break;
    case 0x0BA2: v[0] = 0; v[1] = 0; v[2] = 3840; v[3] = 2160; break;
    case 0x8CA6: v[0] = (int)bound_draw; break;
    case 0x8CAA: v[0] = (int)bound_read; break;
    case 0x8069: v[0] = (int)bound_tex; break;
    case 0x8CA7: v[0] = (int)bound_rb; break;
    case 0x0C02: v[0] = 0x0405; break;
    default: v[0] = 0; }
}
static void g_getUByte(unsigned p, unsigned char *u) { (void)p; for (int i = 0; i < 16; i++) u[i] = (unsigned char)(0xA0 + i); }
static unsigned g_getError(void) { unsigned e = err; err = 0; return e; }
static void g_gen(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = next_id++; }
static void g_del(int n, const unsigned *o) { (void)n; (void)o; }
static void g_bindTexture(unsigned t, unsigned id) { (void)t; bound_tex = id; }
static void g_bindRenderbuffer(unsigned t, unsigned id) { (void)t; bound_rb = id; }
static void g_texParameteri(unsigned a, unsigned b, int c) { (void)a; (void)b; (void)c; }
static void g_bindFramebuffer(unsigned t, unsigned id) { if (t == 0x8CA8 || t == 0x8D40) bound_read = id; if (t == 0x8CA9 || t == 0x8D40) bound_draw = id; }
static unsigned *cur(unsigned t) { return t == 0x8CA8 ? &bound_read : &bound_draw; }
static void g_fbTex2D(unsigned t, unsigned a, unsigned tt, unsigned name, int lvl) { (void)a; (void)tt; (void)lvl; unsigned f = *cur(t); fb[f].color_type = name ? 0x1702 : 0; fb[f].color_name = name; }
static void g_fbRb(unsigned t, unsigned a, unsigned rt, unsigned name) { (void)a; (void)rt; unsigned f = *cur(t); fb[f].color_type = name ? 0x8D41 : 0; fb[f].color_name = name; }
static unsigned g_check(unsigned t) { unsigned f = *cur(t); return (f == 0 || fb[f].color_type) ? 0x8CD5 : 0x8CD6; }
static void g_getFbAtt(unsigned t, unsigned a, unsigned pn, int *v)
{
    (void)a; unsigned f = *cur(t);
    if (pn == 0x8CD0) v[0] = fb[f].color_type; else if (pn == 0x8CD1) v[0] = (int)fb[f].color_name; else if (pn == 0x8CD2) v[0] = 0;
}
static void g_getTexLevel(unsigned t, int l, unsigned pn, int *v) { (void)t; (void)l; v[0] = pn == 0x1000 ? tex[bound_tex].w : tex[bound_tex].h; }
static void g_getRbParam(unsigned t, unsigned pn, int *v) { (void)t; v[0] = pn == 0x8D42 ? rb[bound_rb].w : pn == 0x8D43 ? rb[bound_rb].h : rb[bound_rb].samples; }
static void g_blit(int a, int b, int c, int d, int e, int f, int g, int h, unsigned m, unsigned flt)
{
    (void)m; (void)flt; struct fbo *r = &fb[bound_read];
    T("GL BLIT read_fbo=%u(%s %u) draw_fbo=%u src=(%d,%d,%d,%d) dst=(%d,%d,%d,%d) scissor=%d", bound_read,
      r->color_type == 0x1702 ? "tex" : r->color_type == 0x8D41 ? "rb" : "default", r->color_name, bound_draw, a, b, c, d, e, f, g, h, scissor);
    if (scissor) err = 0x502;
}
static unsigned char g_isTex(unsigned id) { return id < MAXO && tex[id].w > 0; }
static unsigned char g_isRb(unsigned id) { return id < MAXO && rb[id].w > 0; }
static void g_finish(void) { T("GL FINISH"); }
static void g_flush(void) { T("GL FLUSH"); }
static unsigned char g_isEnabled(unsigned c) { return c == 0x0C11 ? (unsigned char)scissor : 0; }
static void g_enable(unsigned c) { if (c == 0x0C11) scissor = 1; }
static void g_disable(unsigned c) { if (c == 0x0C11) scissor = 0; }
/* memory objects / semaphores */
static void g_createMem(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = next_id++; T("GL CreateMemoryObject"); }
static void g_memParam(unsigned m, unsigned p, const int *v) { (void)m; (void)p; (void)v; }
static void g_importMem(unsigned m, uint64_t sz, unsigned ht, int fd) { (void)m; T("GL ImportMemoryFd size=%llu type=0x%x", (unsigned long long)sz, ht); close(fd); }
static void g_texStorageMem(unsigned t, int lv, unsigned ifmt, int w, int h, unsigned m, uint64_t off) { (void)t; (void)lv; (void)ifmt; (void)m; (void)off; tex[bound_tex].w = w; tex[bound_tex].h = h; }
static void g_genSem(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = sem_next++ + 1000; }
static void g_delSem(int n, const unsigned *o) { for (int i = 0; i < n; i++) T("GL DeleteSemaphore %u", o[i]); }
static void g_importSem(unsigned s, unsigned ht, int fd)
{
    T("GL ImportSemaphoreFd sem=%u type=0x%x", s, ht);
    if (getenv("MOCK_SEM_IMPORT_FAIL")) { err = 0x502; return; }   /* on failure the caller keeps the fd */
    close(fd);
}
static void g_waitSem(unsigned s, unsigned nb, const unsigned *b, unsigned nt, const unsigned *t, const unsigned *l) { (void)nb; (void)b; T("GL WaitSemaphore sem=%u tex=%u layout=0x%x (nt=%u)%s", s, nt ? t[0] : 0, nt ? l[0] : 0, nt, ""); if (nt > 1) T("GL   barrier textures: %u %u", t[0], t[1]); }
static void g_sigSem(unsigned s, unsigned nb, const unsigned *b, unsigned nt, const unsigned *t, const unsigned *l) { (void)nb; (void)b; T("GL SignalSemaphore sem=%u tex=%u layout=0x%x (nt=%u)%s", s, nt ? t[0] : 0, nt ? l[0] : 0, nt, ""); if (nt > 1) T("GL   barrier textures: %u %u", t[0], t[1]); }

/* ---- API used by the fake libtogl and the fake game (the "engine" side of GL) ---- */
unsigned mock_make_texture(int w, int h) { unsigned i = next_id++; tex[i].w = w; tex[i].h = h; return i; }
unsigned mock_make_renderbuffer(int w, int h, int samples) { unsigned i = next_id++; rb[i].w = w; rb[i].h = h; rb[i].samples = samples; return i; }
unsigned mock_make_fbo(int type, unsigned name) { unsigned i = next_id++; fb[i].color_type = type; fb[i].color_name = name; return i; }
void mock_bind_draw(unsigned f) { bound_draw = f; }
void mock_set_state(unsigned draw, unsigned read, unsigned t, unsigned r, int sc) { bound_draw = draw; bound_read = read; bound_tex = t; bound_rb = r; scissor = sc; err = 0; }
int mock_state_ok(unsigned draw, unsigned read, unsigned t, unsigned r, int sc) { return bound_draw == draw && bound_read == read && bound_tex == t && bound_rb == r && scissor == sc && err == 0; }
unsigned mock_draw(void) { return bound_draw; }
void mock_trace(const char *s) { T("%s", s); }

struct ent { const char *n; void *f; int sem; };
static struct ent tab[] = {
 {"glGetString", g_getString, 0}, {"glGetStringi", g_getStringi, 0}, {"glGetIntegerv", g_getIntegerv, 0}, {"glGetUnsignedBytevEXT", g_getUByte, 0},
 {"glGenTextures", g_gen, 0}, {"glDeleteTextures", g_del, 0}, {"glBindTexture", g_bindTexture, 0}, {"glTexParameteri", g_texParameteri, 0},
 {"glGenFramebuffers", g_gen, 0}, {"glDeleteFramebuffers", g_del, 0}, {"glBindFramebuffer", g_bindFramebuffer, 0},
 {"glFramebufferTexture2D", g_fbTex2D, 0}, {"glFramebufferRenderbuffer", g_fbRb, 0}, {"glCheckFramebufferStatus", g_check, 0},
 {"glBlitFramebuffer", g_blit, 0}, {"glFinish", g_finish, 0}, {"glFlush", g_flush, 0}, {"glGetError", g_getError, 0},
 {"glIsEnabled", g_isEnabled, 0}, {"glEnable", g_enable, 0}, {"glDisable", g_disable, 0},
 {"glGetFramebufferAttachmentParameteriv", g_getFbAtt, 0}, {"glGetTexLevelParameteriv", g_getTexLevel, 0},
 {"glIsTexture", g_isTex, 0}, {"glIsRenderbuffer", g_isRb, 0}, {"glBindRenderbuffer", g_bindRenderbuffer, 0}, {"glGetRenderbufferParameteriv", g_getRbParam, 0},
 {"glCreateMemoryObjectsEXT", g_createMem, 0}, {"glDeleteMemoryObjectsEXT", g_del, 0}, {"glMemoryObjectParameterivEXT", g_memParam, 0},
 {"glImportMemoryFdEXT", g_importMem, 0}, {"glTexStorageMem2DEXT", g_texStorageMem, 0},
 {"glGenSemaphoresEXT", g_genSem, 1}, {"glDeleteSemaphoresEXT", g_delSem, 1}, {"glImportSemaphoreFdEXT", g_importSem, 1},
 {"glWaitSemaphoreEXT", g_waitSem, 1}, {"glSignalSemaphoreEXT", g_sigSem, 1},
};
void *SDL_GL_GetProcAddress(const char *name)
{
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcmp(tab[i].n, name)) return (tab[i].sem && NOSEM) ? NULL : tab[i].f;
    return NULL;
}
/* keyboard injection: the shim pushes Escape for the Menu button; recorded in the trace */
int SDL_PushEvent(void *ev) { uint32_t type; int32_t sc, sym; memcpy(&type, ev, 4); memcpy(&sc, (char *)ev + 16, 4); memcpy(&sym, (char *)ev + 20, 4); T("SDL_PushEvent type=0x%x scancode=%d sym=%d", type, sc, sym); return 1; }
void *SDL_GetKeyboardFocus(void) { return (void *)1; }
uint32_t SDL_GetWindowID(void *w) { (void)w; return 7; }
void *SDL_GL_GetCurrentContext(void) { return getenv("MOCK_NO_CTX") ? NULL : (void *)1; }
