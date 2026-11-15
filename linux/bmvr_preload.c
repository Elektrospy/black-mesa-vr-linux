/*
 * Black Mesa VR - native Linux port, milestone 1: "prove the hook surface".
 *
 * A 32-bit LD_PRELOAD library for the native Linux Black Mesa (ToGL renderer).
 * It does NOT render anything in VR yet. It checks three things that decide
 * whether the port is viable:
 *
 *   1. LD_PRELOAD interposition of libtogl.so's exported IDirect3DDevice9
 *      methods works (shaderapidx9.so calls them through the PLT).
 *   2. Which render targets / textures the engine creates (names + sizes),
 *      so we can find the stereo eye targets later.
 *   3. Whether the GL driver offers GL_EXT_memory_object_fd and
 *      GL_EXT_semaphore_fd, which the planned GPU hand-off to a 64-bit
 *      OpenXR helper needs.
 *
 * It only observes: every call is forwarded unchanged to the real ToGL.
 * Log file: bmvr_linux_log.txt in the game's working directory.
 *
 * Build (needs gcc -m32 and 32-bit glibc):  scripts/build-linux.sh
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>

typedef int32_t HRESULT;
#define BMVR_EXPORT __attribute__((visibility("default")))

/* ------------------------------------------------------------------ logging */
static int g_log_fd = -2;

static void bmvr_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    if (g_log_fd == -2) {
        g_log_fd = open("bmvr_linux_log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (g_log_fd < 0) return;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (write(g_log_fd, buf, (size_t)n) < 0) { /* logging is best-effort */ }
}

/* ---------------------------------------------------------- symbol forwarding */
static void *real_sym(const char *mangled)
{
    void *p = dlsym(RTLD_NEXT, mangled);
    if (!p) bmvr_log("ERROR: dlsym(RTLD_NEXT, %s) failed - interposition target missing", mangled);
    return p;
}

/* ToGL exports (from libtogl.so, verified in the probe output). */
#define SYM_PRESENT      "_ZN16IDirect3DDevice97PresentEPK5_RECTS2_PvPK7RGNDATA"
#define SYM_SETRT        "_ZN16IDirect3DDevice915SetRenderTargetEjP17IDirect3DSurface9"
#define SYM_STRETCHRECT  "_ZN16IDirect3DDevice911StretchRectEP17IDirect3DSurface9PK5_RECTS1_S4_21_D3DTEXTUREFILTERTYPE"
#define SYM_CREATETEX    "_ZN16IDirect3DDevice913CreateTextureEjjjj10_D3DFORMAT8_D3DPOOLPP17IDirect3DTexture9PPvPc"
#define SYM_CREATERT     "_ZN16IDirect3DDevice918CreateRenderTargetEjj10_D3DFORMAT20_D3DMULTISAMPLE_TYPEjiPP17IDirect3DSurface9PPvPc"
#define SYM_RESET        "_ZN16IDirect3DDevice95ResetEP23_D3DPRESENT_PARAMETERS_"
#define SYM_CREATEDEVICE "_ZN10IDirect3D912CreateDeviceEj11_D3DDEVTYPEPvjP23_D3DPRESENT_PARAMETERS_PP16IDirect3DDevice9"

/* ------------------------------------------------------------ OpenGL probing */
typedef const unsigned char *(*PFN_glGetString)(unsigned);
typedef const unsigned char *(*PFN_glGetStringi)(unsigned, unsigned);
typedef void (*PFN_glGetIntegerv)(unsigned, int *);
typedef void *(*PFN_SDL_GL_GetProcAddress)(const char *);
typedef void *(*PFN_SDL_GL_GetCurrentContext)(void);

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NUM_EXTENSIONS 0x821D
#define GL_EXTENSIONS 0x1F03
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VIEWPORT 0x0BA2

static int g_gl_reported;

static int str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static void report_gl_once(void)
{
    if (g_gl_reported) return;
    PFN_SDL_GL_GetProcAddress getproc = (PFN_SDL_GL_GetProcAddress)dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress");
    PFN_SDL_GL_GetCurrentContext getctx = (PFN_SDL_GL_GetCurrentContext)dlsym(RTLD_DEFAULT, "SDL_GL_GetCurrentContext");
    static unsigned warned;
    if (!getproc || !getctx) {
        if (warned++ < 3) bmvr_log("GL: SDL_GL_* not visible via dlsym (will retry, %u/3)", warned);
        return;
    }
    if (!getctx()) return; /* no current context on this thread yet */

    PFN_glGetString glGetString = (PFN_glGetString)getproc("glGetString");
    PFN_glGetStringi glGetStringi = (PFN_glGetStringi)getproc("glGetStringi");
    PFN_glGetIntegerv glGetIntegerv = (PFN_glGetIntegerv)getproc("glGetIntegerv");
    if (!glGetString || !glGetStringi || !glGetIntegerv) {
        if (warned++ < 3) bmvr_log("GL: entry points missing");
        return;
    }

    g_gl_reported = 1;
    bmvr_log("GL vendor:   %s", glGetString(GL_VENDOR));
    bmvr_log("GL renderer: %s", glGetString(GL_RENDERER));
    bmvr_log("GL version:  %s", glGetString(GL_VERSION));

    int n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    int has_mem = 0, has_memfd = 0, has_sem = 0, has_semfd = 0, has_dma = 0;
    for (int i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (unsigned)i);
        if (!e) continue;
        if (str_eq(e, "GL_EXT_memory_object")) has_mem = 1;
        else if (str_eq(e, "GL_EXT_memory_object_fd")) has_memfd = 1;
        else if (str_eq(e, "GL_EXT_semaphore")) has_sem = 1;
        else if (str_eq(e, "GL_EXT_semaphore_fd")) has_semfd = 1;
        else if (str_eq(e, "GL_EXT_EGL_image_storage")) has_dma = 1;
    }
    bmvr_log("GL extensions: %d total", n);
    bmvr_log("  GL_EXT_memory_object      : %s", has_mem ? "YES" : "no");
    bmvr_log("  GL_EXT_memory_object_fd   : %s   <- needed for Vulkan->GL texture hand-off", has_memfd ? "YES" : "no");
    bmvr_log("  GL_EXT_semaphore          : %s", has_sem ? "YES" : "no");
    bmvr_log("  GL_EXT_semaphore_fd       : %s", has_semfd ? "YES" : "no");
    bmvr_log("  GL_EXT_EGL_image_storage  : %s", has_dma ? "YES" : "no");
    int vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    bmvr_log("GL viewport at first Present: %d %d %d %d", vp[0], vp[1], vp[2], vp[3]);
}

/* ------------------------------------------------------------------ counters */
static unsigned g_frames, g_tex_calls, g_rt_calls, g_setrt_calls, g_stretch_calls;
static uint64_t g_last_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------ constructor */
__attribute__((constructor)) static void bmvr_init(void)
{
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0) n = 0;
    exe[n] = 0;
    bmvr_log("bmvr_preload loaded, pid=%d exe=%s", getpid(), exe);
    bmvr_log("milestone 1: observe-only. All calls forwarded to libtogl.so unchanged.");
}

/* =========================================================== interposed API =
 * i386 Itanium ABI: member functions are plain cdecl with `this` as the first
 * stack argument, so a C function with an explicit `self` parameter and an asm
 * label matching the mangled name replaces the C++ member function exactly.
 */

typedef HRESULT (*PFN_Present)(void *, const void *, const void *, void *, const void *);
BMVR_EXPORT HRESULT bmvr_Present(void *self, const void *src, const void *dst, void *hwnd, const void *dirty)
    __asm__(SYM_PRESENT);
BMVR_EXPORT HRESULT bmvr_Present(void *self, const void *src, const void *dst, void *hwnd, const void *dirty)
{
    static PFN_Present real;
    if (!real) real = (PFN_Present)real_sym(SYM_PRESENT);
    if (g_frames == 0) bmvr_log("Present: first call (device=%p) - interposition works", self);
    report_gl_once();
    g_frames++;
    uint64_t t = now_ns();
    if (!g_last_ns) g_last_ns = t;
    if (t - g_last_ns >= 5000000000ull) {
        bmvr_log("stats: %.1f fps | calls in last window: CreateTexture=%u CreateRenderTarget=%u SetRenderTarget=%u StretchRect=%u",
             (double)g_frames * 1e9 / (double)(t - g_last_ns), g_tex_calls, g_rt_calls, g_setrt_calls, g_stretch_calls);
        g_frames = 1; g_tex_calls = g_rt_calls = g_setrt_calls = g_stretch_calls = 0; g_last_ns = t;
    }
    return real ? real(self, src, dst, hwnd, dirty) : -1;
}

typedef HRESULT (*PFN_SetRT)(void *, unsigned, void *);
BMVR_EXPORT HRESULT bmvr_SetRenderTarget(void *self, unsigned idx, void *surf) __asm__(SYM_SETRT);
BMVR_EXPORT HRESULT bmvr_SetRenderTarget(void *self, unsigned idx, void *surf)
{
    static PFN_SetRT real;
    if (!real) real = (PFN_SetRT)real_sym(SYM_SETRT);
    g_setrt_calls++;
    return real ? real(self, idx, surf) : -1;
}

typedef HRESULT (*PFN_Stretch)(void *, void *, const void *, void *, const void *, unsigned);
BMVR_EXPORT HRESULT bmvr_StretchRect(void *self, void *src, const void *sr, void *dst, const void *dr, unsigned filter)
    __asm__(SYM_STRETCHRECT);
BMVR_EXPORT HRESULT bmvr_StretchRect(void *self, void *src, const void *sr, void *dst, const void *dr, unsigned filter)
{
    static PFN_Stretch real;
    if (!real) real = (PFN_Stretch)real_sym(SYM_STRETCHRECT);
    g_stretch_calls++;
    return real ? real(self, src, sr, dst, dr, filter) : -1;
}

typedef HRESULT (*PFN_CreateTex)(void *, unsigned, unsigned, unsigned, unsigned, int, int, void **, void **, char *);
BMVR_EXPORT HRESULT bmvr_CreateTexture(void *self, unsigned w, unsigned h, unsigned levels, unsigned usage,
                                       int fmt, int pool, void **ppTex, void **ppShared, char *label)
    __asm__(SYM_CREATETEX);
BMVR_EXPORT HRESULT bmvr_CreateTexture(void *self, unsigned w, unsigned h, unsigned levels, unsigned usage,
                                       int fmt, int pool, void **ppTex, void **ppShared, char *label)
{
    static PFN_CreateTex real;
    static unsigned logged;
    if (!real) real = (PFN_CreateTex)real_sym(SYM_CREATETEX);
    g_tex_calls++;
    HRESULT hr = real ? real(self, w, h, levels, usage, fmt, pool, ppTex, ppShared, label) : -1;
    /* Render-target usage is D3DUSAGE_RENDERTARGET (0x1). Log those and any big texture. */
    if (logged < 400 && ((usage & 1u) || w >= 1024 || h >= 1024)) {
        logged++;
        bmvr_log("CreateTexture %ux%u levels=%u usage=0x%x fmt=%d pool=%d -> %p hr=%d label=%s",
             w, h, levels, usage, fmt, pool, (ppTex ? *ppTex : 0), hr, label ? label : "(null)");
    }
    return hr;
}

typedef HRESULT (*PFN_CreateRT)(void *, unsigned, unsigned, int, int, unsigned, int, void **, void **, char *);
BMVR_EXPORT HRESULT bmvr_CreateRenderTarget(void *self, unsigned w, unsigned h, int fmt, int ms, unsigned msq,
                                            int lockable, void **ppSurf, void **ppShared, char *label)
    __asm__(SYM_CREATERT);
BMVR_EXPORT HRESULT bmvr_CreateRenderTarget(void *self, unsigned w, unsigned h, int fmt, int ms, unsigned msq,
                                            int lockable, void **ppSurf, void **ppShared, char *label)
{
    static PFN_CreateRT real;
    static unsigned logged;
    if (!real) real = (PFN_CreateRT)real_sym(SYM_CREATERT);
    g_rt_calls++;
    HRESULT hr = real ? real(self, w, h, fmt, ms, msq, lockable, ppSurf, ppShared, label) : -1;
    if (logged < 200) {
        logged++;
        bmvr_log("CreateRenderTarget %ux%u fmt=%d ms=%d -> %p hr=%d label=%s",
             w, h, fmt, ms, (ppSurf ? *ppSurf : 0), hr, label ? label : "(null)");
    }
    return hr;
}

typedef HRESULT (*PFN_Reset)(void *, void *);
BMVR_EXPORT HRESULT bmvr_Reset(void *self, void *params) __asm__(SYM_RESET);
BMVR_EXPORT HRESULT bmvr_Reset(void *self, void *params)
{
    static PFN_Reset real;
    if (!real) real = (PFN_Reset)real_sym(SYM_RESET);
    bmvr_log("Reset called (device=%p)", self);
    return real ? real(self, params) : -1;
}

typedef HRESULT (*PFN_CreateDevice)(void *, unsigned, int, void *, unsigned, void *, void **);
BMVR_EXPORT HRESULT bmvr_CreateDevice(void *self, unsigned adapter, int devtype, void *hwnd, unsigned behavior,
                                      void *params, void **ppDevice) __asm__(SYM_CREATEDEVICE);
BMVR_EXPORT HRESULT bmvr_CreateDevice(void *self, unsigned adapter, int devtype, void *hwnd, unsigned behavior,
                                      void *params, void **ppDevice)
{
    static PFN_CreateDevice real;
    if (!real) real = (PFN_CreateDevice)real_sym(SYM_CREATEDEVICE);
    HRESULT hr = real ? real(self, adapter, devtype, hwnd, behavior, params, ppDevice) : -1;
    bmvr_log("IDirect3D9::CreateDevice adapter=%u devtype=%d behavior=0x%x -> device=%p hr=%d",
         adapter, devtype, behavior, (ppDevice ? *ppDevice : 0), hr);
    return hr;
}
