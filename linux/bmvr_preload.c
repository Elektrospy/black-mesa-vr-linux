/*
 * Black Mesa VR - native Linux port, milestone 1 ("prove the hook surface") + stage 2a/2b (GPU hand-off).
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
#include <sys/time.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include "bmvr_xfer.h"
#include "bmvr_stereo.h"
#include "bmvr_rtti.h"
#include "bmvr_inline.h"
#include "bmvr_input_map.h"

/* Portability: this library is loaded into the game, which Steam may run inside an OLDER
 * glibc (Steam Linux Runtime). Built on a modern distro it would require dlsym@GLIBC_2.34
 * and clock_gettime@GLIBC_2.17; the dynamic loader then silently skips the whole preload
 * library (no error, no log). So: pin dlsym to its oldest version, avoid clock_gettime. */
#if defined(__i386__)
__asm__(".symver dlsym,dlsym@GLIBC_2.0");
__asm__(".symver dlopen,dlopen@GLIBC_2.1");
#endif

typedef int32_t HRESULT;
#define BMVR_EXPORT __attribute__((visibility("default")))

/* ------------------------------------------------------------------ logging */
static int g_log_fd = -2;
static char g_log_path[512];

static int try_open_log(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) snprintf(g_log_path, sizeof g_log_path, "%s", path);
    return fd;
}

/* Preferred: next to the game executable. Fallbacks make sure a log shows up somewhere. */
static void open_log_once(void)
{
    char dir[400], path[512];
    g_log_fd = -1;
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n > 0) {
        dir[n] = 0;
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            snprintf(path, sizeof path, "%s/bmvr_linux_log.txt", dir);
            g_log_fd = try_open_log(path);
        }
    }
    if (g_log_fd < 0) g_log_fd = try_open_log("bmvr_linux_log.txt"); /* cwd */
    if (g_log_fd < 0) {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(path, sizeof path, "%s/bmvr_linux_log.txt", home);
            g_log_fd = try_open_log(path);
        }
    }
    if (g_log_fd < 0) g_log_fd = try_open_log("/tmp/bmvr_linux_log.txt");
    {
        char msg[640];
        int m = snprintf(msg, sizeof msg, "bmvr_preload: log file = %s\n",
                         g_log_fd >= 0 ? g_log_path : "(could not open any log location)");
        if (m > 0 && write(2, msg, (size_t)m) < 0) { /* best effort */ }
    }
}

static void bmvr_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    if (g_log_fd == -2) open_log_once();
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
/* libtogl.so is usually loaded by the game with dlopen(..., RTLD_LOCAL) (via shaderapidx9.so), and
 * dlsym(RTLD_NEXT, ...) from a preload library cannot see such a library. So look it up explicitly:
 * first by name (RTLD_NOLOAD = only if already loaded, never loads anything), then by the full path
 * from /proc/self/maps (the game folder has a space in it, so take the rest of the line). */
#ifndef BMVR_TOGL_NAME
#define BMVR_TOGL_NAME "libtogl.so"
#endif
static void *g_togl;
static int g_togl_tried;

/* Handle of an ALREADY LOADED library (never loads anything): by soname first, then by the path found
 * in /proc/self/maps (line text after the first '/', so folders with spaces work). */
static void *find_loaded_lib(const char *soname, const char *maps_substr)
{
    void *h = dlopen(soname, RTLD_LAZY | RTLD_NOLOAD);
    if (h) return h;
    FILE *f = fopen("/proc/self/maps", "r");
    char line[1024];
    while (f && fgets(line, sizeof line, f)) {
        char *start = strchr(line, '/');
        if (!start || !strstr(start, maps_substr)) continue;
        size_t n = strlen(start);
        while (n && (start[n - 1] == '\n' || start[n - 1] == '\r')) start[--n] = 0;
        h = dlopen(start, RTLD_LAZY | RTLD_NOLOAD);
        if (h) { bmvr_log("found %s via /proc/self/maps: %s", soname, start); break; }
    }
    if (f) fclose(f);
    return h;
}

static void *togl_handle(void)
{
    if (!g_togl) g_togl = find_loaded_lib(BMVR_TOGL_NAME, "/" BMVR_TOGL_NAME);
    if (!g_togl_tried) {
        g_togl_tried = 1;
        bmvr_log("ToGL handle (%s): %p", BMVR_TOGL_NAME, g_togl);
    }
    return g_togl;
}

static void *real_sym(const char *mangled)
{
    void *p = dlsym(RTLD_NEXT, mangled);
    if (!p) {
        void *h = togl_handle();
        if (h) p = dlsym(h, mangled);
    }
    if (!p) bmvr_log("ERROR: real %s not found (RTLD_NEXT and libtogl handle %p) - interposition target missing",
                     mangled, togl_handle());
    return p;
}

/* ToGL exports (from libtogl.so, verified in the probe output). */
#define SYM_PRESENT      "_ZN16IDirect3DDevice97PresentEPK5_RECTS2_PvPK7RGNDATA"
#define SYM_SETRT        "_ZN16IDirect3DDevice915SetRenderTargetEjP17IDirect3DSurface9"
#define SYM_STRETCHRECT  "_ZN16IDirect3DDevice911StretchRectEP17IDirect3DSurface9PK5_RECTS1_S4_21_D3DTEXTUREFILTERTYPE"
#define SYM_CREATETEX    "_ZN16IDirect3DDevice913CreateTextureEjjjj10_D3DFORMAT8_D3DPOOLPP17IDirect3DTexture9PPvPc"
#define SYM_CREATERT     "_ZN16IDirect3DDevice918CreateRenderTargetEjj10_D3DFORMAT20_D3DMULTISAMPLE_TYPEjiPP17IDirect3DSurface9PPvPc"
#define SYM_RESET        "_ZN16IDirect3DDevice95ResetEP23_D3DPRESENT_PARAMETERS_"
/* Stage 2b: NOT confirmed to be exported by libtogl.so (the 2026-10 probe listing was truncated). If it is
 * not, this interposer is simply never called; the Present stats line then shows surfLevel=0 and render
 * targets can only be selected by size (BMVR_TARGET=size:WxH). */
#define SYM_GETSURF      "_ZN17IDirect3DTexture915GetSurfaceLevelEjPP17IDirect3DSurface9"
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
static PFN_SDL_GL_GetProcAddress g_getproc; /* set once the GL report succeeded; used by the xfer code */
static void *(*g_getctx_fn)(void);          /* SDL_GL_GetCurrentContext, set together with g_getproc */

static int str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static void report_gl_once(void)
{
    if (g_gl_reported) return;
    PFN_SDL_GL_GetProcAddress getproc = (PFN_SDL_GL_GetProcAddress)dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress");
    PFN_SDL_GL_GetCurrentContext getctx = (PFN_SDL_GL_GetCurrentContext)dlsym(RTLD_DEFAULT, "SDL_GL_GetCurrentContext");
    static unsigned warned;
    static void *sdl;
    if (!getproc || !getctx) {
        /* SDL is usually dlopen'ed RTLD_LOCAL by the engine: look in its own handle. */
        if (!sdl) sdl = find_loaded_lib("libSDL2-2.0.so.0", "libSDL2");
        if (sdl) {
            getproc = (PFN_SDL_GL_GetProcAddress)dlsym(sdl, "SDL_GL_GetProcAddress");
            getctx = (PFN_SDL_GL_GetCurrentContext)dlsym(sdl, "SDL_GL_GetCurrentContext");
        }
    }
    if (!getproc || !getctx) {
        if (warned++ < 3) bmvr_log("GL: SDL_GL_* not found (SDL handle %p, try %u/3)", sdl, warned);
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
    g_getproc = getproc;
    g_getctx_fn = getctx;
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
    if (has_memfd) { /* must equal the helper's Vulkan device UUID, or the memory cannot be shared */
        typedef void (*PFN_glGetUnsignedBytevEXT)(unsigned, unsigned char *);
        PFN_glGetUnsignedBytevEXT getub = (PFN_glGetUnsignedBytevEXT)getproc("glGetUnsignedBytevEXT");
        if (getub) {
            unsigned char u[16];
            char s[40];
            memset(u, 0, sizeof u);
            getub(0x9597 /* GL_DEVICE_UUID_EXT */, u);
            for (int i = 0; i < 16; i++) snprintf(s + i * 2, 3, "%02x", u[i]);
            bmvr_log("GL device UUID: %s", s);
        }
    }
    int vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    bmvr_log("GL viewport at first Present: %d %d %d %d", vp[0], vp[1], vp[2], vp[3]);
}

/* ============================================================ stage 2a/2b: GPU hand-off
 * Optional and passive: nothing is sent unless the 64-bit helper (bmvr_xfer_helper) is running
 * and has created "<game folder>/bmvr_xfer.sock". See linux/bmvr_xfer.h for the protocol.
 *
 * 2a copied the window's default framebuffer. 2b copies an ENGINE RENDER TARGET (the eye targets
 * of the later stages are such targets). Source selection ("target spec"), in priority order:
 * env BMVR_TARGET, then "<game folder>/bmvr_target.txt" (line "target=..." or just the spec):
 *     name:<substring>   render target whose engine label contains it (case-insensitive).
 *                        DEFAULT: name:_rt_fullframefb
 *     size:WxH           any render target whose GL attachment is exactly that size
 *     window             the default framebuffer, as in stage 2a
 * and "flip=0|1|alt" (env BMVR_FLIP). Default alt: successive snapshots alternate the Y flip, and
 * the helper puts the flip into the file name, so one run shows which orientation is upright.
 *
 * How the GL texture of a D3D render target is found without knowing ToGL's private structs:
 *   - CreateTexture / CreateRenderTarget / GetSurfaceLevel are interposed, so every surface the
 *     engine can bind has a label and a size in a small registry.
 *   - ToGL binds the GL framebuffer lazily, at draw time. So when the engine calls SetRenderTarget(0, X)
 *     the GL DRAW framebuffer still holds what the engine drew into BEFORE: the target it is
 *     leaving. At that moment (before forwarding) we ask GL what is attached to it
 *     (glGetFramebufferAttachmentParameteriv: type + name), and measure the attachment.
 *   - The answer is trusted only if the measured size equals the registry size of the target being
 *     left (guards against "no draw happened since the last SetRenderTarget" and stale pointers).
 *   - We remember the LAST such leave of the frame (type, name, size). At the next Present the
 *     helper's shared texture is filled from a private read FBO that attaches that GL object. So the
 *     snapshot is the target's final content of the frame, not a mid-frame state.
 * Only the frame before each snapshot is "armed", so the cost elsewhere is a pointer compare.
 *
 * "rt-trace": twice per session (and without the helper) one full frame of SetRenderTarget(0)
 * calls is logged with what GL reports for the target being left. This is the discovery tool for
 * the real eye targets: it shows the order of render targets in a frame and which ones GL can
 * resolve.
 *
 * Safety: if anything fails the shim logs "xfer: ..." and goes back to doing nothing; the game is
 * never blocked for longer than the 2 s socket timeout; GL state touched here (FBO bindings,
 * texture/renderbuffer binding, scissor) is saved and restored and GL errors raised by our own
 * calls are drained so the engine never sees them. Everything runs only on a thread that has a
 * current GL context. */
#define XFER_EVERY_DEFAULT 120  /* presents between deliveries (~2 s at 60 fps): the snapshot-era default */
static unsigned g_x_every = XFER_EVERY_DEFAULT; /* bmvr_target.txt "every=N": 1 = deliver on every present (needed for a live picture in a headset) */
#define XFER_RETRY 240          /* presents between attempts to find the helper */
#define XFER_TIMEOUT_SEC 2
#define TRACE_LINES 1500        /* SetRenderTarget events traced per frame (a gameplay frame has ~400; identical neighbours are merged in the log) */
#define TRACE_MIN_BUSY 50       /* a frame counts as gameplay when it made at least this many SetRenderTarget(0) calls */

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_LINEAR 0x2601
#define GL_SCISSOR_TEST 0x0C11
#define GL_READ_BUFFER 0x0C02
#define GL_TEXTURE_TILING_EXT 0x9580
#define GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581
#define GL_OPTIMAL_TILING_EXT 0x9584
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586
#define GL_LAYOUT_GENERAL_EXT 0x958D
#define GL_RENDERBUFFER 0x8D41
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_RENDERBUFFER_WIDTH 0x8D42
#define GL_RENDERBUFFER_HEIGHT 0x8D43
#define GL_RENDERBUFFER_SAMPLES 0x8CAB
#define GL_TEXTURE_OBJ 0x1702   /* GL_TEXTURE: value of FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE for a texture */
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1

typedef void (*PFN_glCreateMemoryObjectsEXT)(int, unsigned *);
typedef void (*PFN_glDeleteMemoryObjectsEXT)(int, const unsigned *);
typedef void (*PFN_glMemoryObjectParameterivEXT)(unsigned, unsigned, const int *);
typedef void (*PFN_glImportMemoryFdEXT)(unsigned, uint64_t, unsigned, int);
typedef void (*PFN_glTexStorageMem2DEXT)(unsigned, int, unsigned, int, int, unsigned, uint64_t);
typedef void (*PFN_glGenTextures)(int, unsigned *);
typedef void (*PFN_glDeleteTextures)(int, const unsigned *);
typedef void (*PFN_glBindTexture)(unsigned, unsigned);
typedef void (*PFN_glTexParameteri)(unsigned, unsigned, int);
typedef void (*PFN_glGenFramebuffers)(int, unsigned *);
typedef void (*PFN_glDeleteFramebuffers)(int, const unsigned *);
typedef void (*PFN_glBindFramebuffer)(unsigned, unsigned);
typedef void (*PFN_glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
typedef void (*PFN_glFramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
typedef unsigned (*PFN_glCheckFramebufferStatus)(unsigned);
typedef void (*PFN_glBlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
typedef void (*PFN_glFinish)(void);
typedef void (*PFN_glFlush)(void);
typedef void (*PFN_glGenSemaphoresEXT)(int, unsigned *);
typedef void (*PFN_glDeleteSemaphoresEXT)(int, const unsigned *);
typedef void (*PFN_glImportSemaphoreFdEXT)(unsigned, unsigned, int);
typedef void (*PFN_glSemaphoreBarriersEXT)(unsigned, unsigned, const unsigned *, unsigned, const unsigned *, const unsigned *);  /* glWait/SignalSemaphoreEXT */
typedef unsigned (*PFN_glGetError)(void);
typedef unsigned char (*PFN_glIsEnabled)(unsigned);
typedef void (*PFN_glEnable)(unsigned);
typedef void (*PFN_glDisable)(unsigned);
typedef void (*PFN_glGetFramebufferAttachmentParameteriv)(unsigned, unsigned, unsigned, int *);
typedef void (*PFN_glGetTexLevelParameteriv)(unsigned, int, unsigned, int *);
typedef void (*PFN_glBindRenderbuffer)(unsigned, unsigned);
typedef void (*PFN_glGetRenderbufferParameteriv)(unsigned, unsigned, int *);
typedef unsigned char (*PFN_glIsTexture)(unsigned);
typedef unsigned char (*PFN_glIsRenderbuffer)(unsigned);

static struct {
    PFN_glCreateMemoryObjectsEXT CreateMemoryObjects;
    PFN_glDeleteMemoryObjectsEXT DeleteMemoryObjects;
    PFN_glMemoryObjectParameterivEXT MemoryObjectParameteriv;
    PFN_glImportMemoryFdEXT ImportMemoryFd;
    PFN_glTexStorageMem2DEXT TexStorageMem2D;
    PFN_glGenTextures GenTextures;
    PFN_glDeleteTextures DeleteTextures;
    PFN_glBindTexture BindTexture;
    PFN_glTexParameteri TexParameteri;
    PFN_glGenFramebuffers GenFramebuffers;
    PFN_glDeleteFramebuffers DeleteFramebuffers;
    PFN_glBindFramebuffer BindFramebuffer;
    PFN_glFramebufferTexture2D FramebufferTexture2D;
    PFN_glFramebufferRenderbuffer FramebufferRenderbuffer;
    PFN_glCheckFramebufferStatus CheckFramebufferStatus;
    PFN_glBlitFramebuffer BlitFramebuffer;
    PFN_glFinish Finish;
    PFN_glFlush Flush;
    PFN_glGenSemaphoresEXT GenSemaphores;          /* GL_EXT_semaphore + GL_EXT_semaphore_fd (stage 3a, optional) */
    PFN_glDeleteSemaphoresEXT DeleteSemaphores;
    PFN_glImportSemaphoreFdEXT ImportSemaphoreFd;
    PFN_glSemaphoreBarriersEXT WaitSemaphore;
    PFN_glSemaphoreBarriersEXT SignalSemaphore;
    PFN_glGetError GetError;
    PFN_glIsEnabled IsEnabled;
    PFN_glEnable Enable;
    PFN_glDisable Disable;
    PFN_glGetIntegerv GetIntegerv;
    PFN_glGetFramebufferAttachmentParameteriv GetFramebufferAttachmentParameteriv;
    PFN_glGetTexLevelParameteriv GetTexLevelParameteriv;
    PFN_glBindRenderbuffer BindRenderbuffer;
    PFN_glGetRenderbufferParameteriv GetRenderbufferParameteriv;
    PFN_glIsTexture IsTexture;                  /* optional: used to check that a remembered object still exists */
    PFN_glIsRenderbuffer IsRenderbuffer;        /* optional */
} xgl;

static int g_x_state;            /* 0 = looking for the helper, 1 = connected, -1 = given up */
static int g_x_sock = -1;
static unsigned g_x_tick;
static unsigned g_x_snapshots;
static uint32_t g_x_w, g_x_h;
#define XFER_EYES 2                          /* stage 3d: shared images per connection (eye 0 = the single image of earlier stages) */
static unsigned g_x_mem[XFER_EYES], g_x_tex[XFER_EYES], g_x_fbo[XFER_EYES];   /* per eye: imported memory, its texture, FBO that draws into it */
static unsigned g_x_neyes = 1;               /* eyes in use on this connection (1, or 2 when target2 is set and the helper offers two images) */
static unsigned g_x_rfbo;                    /* private FBO used as the blit SOURCE (attaches the target) */
/* Stage 3a: GPU semaphores instead of glFinish. */
static int g_sync_want;                      /* config: 0 = glFinish + ack (default, verified), 1 = GPU semaphores */
static int g_x_poses_on;                     /* v5: the helper offers poses (its acks carry one) */
static struct bmvr_xfer_pose g_x_pose_latest;/* the pose of the newest ack that had one (pose_id 0 = none yet) */
static struct bmvr_xfer_pose g_x_pose_pending_data; /* the pose data of g_x_pose_pending (g_x_pose_latest moves on with every ack) */
static uint32_t g_x_pose_pending, g_x_pose_used;   /* pose id for the NEXT frame / for the frame being delivered now (see xfer_tick) */
static int g_x_sem_on;                       /* semaphores imported and in use for this connection */
static int g_x_sem_inflight;                 /* a frame was published: the next blit must first wait for vk_done */
static unsigned g_x_sem_gl_ready, g_x_sem_vk_done;   /* GL semaphore names (imported from the helper) */
static uint64_t g_x_t0;                      /* when this snapshot started (cost measurement) */
static uint64_t g_x_cost_gl_us, g_x_cost_rtt_us;     /* running sums over delivered snapshots */
static unsigned g_x_cost_n;
static uint64_t now_ns(void);

/* ------------------------------------------------------------ target spec / config */
enum { SPEC_NAME = 0, SPEC_SIZE = 1, SPEC_WINDOW = 2 };
struct tspec { int kind; char name[40]; unsigned w, h; };
/* [0] = eye 0 = the target of the earlier stages; [1] = eye 1 (stage 3d), in use only when "target2=" / BMVR_TARGET2 is set. */
static struct tspec g_spec[XFER_EYES] = { { SPEC_NAME, "_rt_fullframefb", 0, 0 }, { SPEC_NAME, "", 0, 0 } };
static int g_spec2_on;           /* target2 configured */
static int g_flip_mode = 2;      /* 0 / 1 = fixed, 2 = alternate per snapshot */
static int g_flip2_mode = -1;    /* eye 1: -1 = same as eye 0, else as g_flip_mode */
static int g_cfg_loaded;

/* Stage 4b: stereo views (docs/LINUX_PORT.md, "Stage 4b"). All opt-in: nothing below does anything unless "stereo=1" is set. */
static int g_st_want;                              /* config: stereo=1 (BMVR_STEREO=1) */
static char g_st_module[96] = "/client.so";        /* stereo_module=: which loaded module holds the view class ("exe" = the main program) */
static char g_st_class[64] = "CViewRender";        /* stereo_class=: class whose vtable holds RenderView (plain name; mangled here) */
static int g_st_class_explicit;                    /* stereo_class= was given: never substitute another class */
static int g_st_slot = 6;                          /* stereo_slot=: vtable slot of RenderView. 6 on the Windows build (IViewRender); UNVERIFIED on Linux */
static int g_st_hook_mode;                         /* stereo_hook=: 0 auto (vtable slot, then an inline detour if the slot is never called), 1 vtable only, 2 inline only */
static float g_st_scale = BMVR_STEREO_DEFAULT_SCALE; /* stereo_scale=: Source units per metre */

/* Slice 3b: controller input into the game (docs/LINUX_PORT.md). Opt-in: nothing below does anything unless "input=1" is set. The hook is on
 * IClientMode::CreateMove (ClientModeShared slot 21 on the Windows build; the Linux build has two destructor slots, so 22 is the first guess:
 * UNVERIFIED, the learning phase below says whether it is right before a single value is written). */
static int g_ic_want;                              /* config: input=1 (BMVR_INPUT=1) */
static char g_ic_module[96] = "/client.so";        /* input_module= */
static char g_ic_class[64] = "ClientModeShared";   /* input_class=: class whose vtable holds CreateMove (plain name; mangled here) */
static int g_ic_class_explicit;
static int g_ic_probe = 1;                        /* input_probe=0 turns off watching the subclasses' vtables too */
static int g_ic_slot = 22;                         /* input_slot= */
static float g_ic_turn_speed = 180.0f;             /* input_turn_speed= degrees per second at full stick (smooth turning) */
static int g_ic_turn_analog = 1;                   /* input_turn_analog=0: one constant speed past the deadzone; default 1: the speed scales with the stick's deflection */
static float g_ic_snap = 0.0f;                     /* input_snap= degrees per flick; 0 = smooth turning */
static int g_ic_weapon_left;                       /* input_weapon_hand=left: mirror the controllers (weapon in the left hand, walk on the right stick) */
static int g_ic_move_hand = -1;                    /* input_move_hand=left|right; -1 = the hand opposite the weapon hand */
static float g_ic_deadzone = 0.15f;                /* input_deadzone= walk stick dead zone, 0.05..0.5 */
static float g_ic_trigger = 0.55f;                 /* input_trigger= trigger press point, 0.2..0.9 */
static int g_ic_act[5] = { BMVR_ACT_RELOAD, BMVR_ACT_ATTACK2, BMVR_ACT_JUMP, BMVR_ACT_DUCK, BMVR_ACT_NONE };   /* input_btn_a / b / bumper / x / y */
static volatile int g_st_recenter_req;             /* the right stick click asks the stereo rig to recenter on its next frame */

static int game_dir(char *out, size_t cap)
{
    char exe[400];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) return 0;
    *slash = 0;
    snprintf(out, cap, "%s", exe);
    return 1;
}

/* Case-insensitive substring test; own implementation so no extra glibc symbol version is needed. */
static int ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i]) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == nl) return 1;
    }
    return 0;
}

static void trim_eol(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

/* Decimal parser. Not strtoul: with _GNU_SOURCE a modern glibc links strtoul to __isoc23_strtoul@GLIBC_2.38,
 * which an older Steam runtime lacks, and the dynamic loader would then silently skip this whole library. */
static unsigned parse_uint(const char *s, const char **end)
{
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') { if (v < 100000u) v = v * 10u + (unsigned)(*s - '0'); s++; }
    *end = s;
    return v;
}

static void parse_spec(struct tspec *sp, const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    if (!strncmp(s, "window", 6)) { sp->kind = SPEC_WINDOW; return; }
    if (!strncmp(s, "size:", 5)) {
        const char *e1 = NULL, *e2 = NULL;
        unsigned w = parse_uint(s + 5, &e1), h = 0;
        if (e1 && (*e1 == 'x' || *e1 == 'X')) h = parse_uint(e1 + 1, &e2);
        if (w && h && w <= 16384 && h <= 16384) { sp->kind = SPEC_SIZE; sp->w = w; sp->h = h; return; }
        bmvr_log("xfer: bad target spec '%s' (want size:WxH); keeping the previous one", s);
        return;
    }
    if (!strncmp(s, "name:", 5)) s += 5;
    if (!*s) return;
    snprintf(sp->name, sizeof sp->name, "%s", s);
    sp->kind = SPEC_NAME;
}

/* target2 = the second eye. Only a render target (by label or size) makes sense: eye 0 may be the window only when
 * there is no second eye. */
static void parse_spec2(const char *s)
{
    struct tspec t = g_spec[1];
    parse_spec(&t, s);
    if (t.kind == SPEC_WINDOW) { bmvr_log("xfer: target2=window is not supported (eye 1 must be a render target); ignored"); return; }
    g_spec[1] = t;
    g_spec2_on = (t.kind == SPEC_SIZE) || (t.kind == SPEC_NAME && t.name[0]);
}

static void parse_sync(const char *s)
{
    if (!strncmp(s, "sem", 3)) g_sync_want = 1;
    else if (!strncmp(s, "fin", 3)) g_sync_want = 0;
    else bmvr_log("xfer: bad sync value '%s' (want sem or finish); keeping %s", s, g_sync_want ? "sem" : "finish");
}

static int flip_value(const char *s, int keep)
{
    if (!strncmp(s, "alt", 3)) return 2;
    if (s[0] == '0') return 0;
    if (s[0] == '1') return 1;
    return keep;
}
static void parse_flip(const char *s) { g_flip_mode = flip_value(s, g_flip_mode); }
static void parse_flip2(const char *s) { g_flip2_mode = flip_value(s, g_flip2_mode); }

static void load_cfg_once(void)
{
    if (g_cfg_loaded) return;
    g_cfg_loaded = 1;
    char dir[400], path[512], line[200];
    if (game_dir(dir, sizeof dir)) {
        snprintf(path, sizeof path, "%s/bmvr_target.txt", dir);
        FILE *f = fopen(path, "r");
        int first = 1;
        while (f && fgets(line, sizeof line, f)) {
            trim_eol(line);
            if (!line[0] || line[0] == '#') continue;
            if (!strncmp(line, "target=", 7)) parse_spec(&g_spec[0], line + 7);
            else if (!strncmp(line, "target2=", 8)) parse_spec2(line + 8);
            else if (!strncmp(line, "flip=", 5)) parse_flip(line + 5);
            else if (!strncmp(line, "flip2=", 6)) parse_flip2(line + 6);
            else if (!strncmp(line, "sync=", 5)) parse_sync(line + 5);
            else if (!strncmp(line, "stereo=", 7)) g_st_want = line[7] == '1';
            else if (!strncmp(line, "stereo_module=", 14)) snprintf(g_st_module, sizeof g_st_module, "%.95s", line + 14);
            else if (!strncmp(line, "stereo_class=", 13)) { snprintf(g_st_class, sizeof g_st_class, "%.63s", line + 13); g_st_class_explicit = 1; }
            else if (!strncmp(line, "stereo_hook=", 12)) g_st_hook_mode = !strcmp(line + 12, "vtable") ? 1 : !strcmp(line + 12, "inline") ? 2 : 0;
            else if (!strncmp(line, "stereo_slot=", 12)) {
                long v = strtol(line + 12, NULL, 10);
                if (v >= 0 && v < 256) g_st_slot = (int)v;
                else bmvr_log("stereo: bad stereo_slot=%s (use 0..255); keeping %d", line + 12, g_st_slot);
            }
            else if (!strncmp(line, "stereo_scale=", 13)) {
                long v = strtol(line + 13, NULL, 10);
                if (v >= 2 && v <= 400) g_st_scale = (float)v;
                else bmvr_log("stereo: bad stereo_scale=%s (use a whole number 2..400 units per metre); keeping %d", line + 13, (int)g_st_scale);
            }
            else if (!strncmp(line, "input=", 6)) g_ic_want = line[6] == '1';
            else if (!strncmp(line, "input_module=", 13)) snprintf(g_ic_module, sizeof g_ic_module, "%.95s", line + 13);
            else if (!strncmp(line, "input_class=", 12)) { snprintf(g_ic_class, sizeof g_ic_class, "%.63s", line + 12); g_ic_class_explicit = 1; }
            else if (!strncmp(line, "input_turn_analog=", 18)) g_ic_turn_analog = line[18] != '0';
            else if (!strncmp(line, "input_weapon_hand=", 18)) g_ic_weapon_left = !strcmp(line + 18, "left");
            else if (!strncmp(line, "input_move_hand=", 16)) g_ic_move_hand = !strcmp(line + 16, "left") ? 0 : !strcmp(line + 16, "right") ? 1 : -1;
            else if (!strncmp(line, "input_deadzone=", 15)) { float v = (float)atof(line + 15); if (v >= 0.05f && v <= 0.5f) g_ic_deadzone = v; else bmvr_log("input: bad input_deadzone=%s (use 0.05..0.5)", line + 15); }
            else if (!strncmp(line, "input_trigger=", 14)) { float v = (float)atof(line + 14); if (v >= 0.2f && v <= 0.9f) g_ic_trigger = v; else bmvr_log("input: bad input_trigger=%s (use 0.2..0.9)", line + 14); }
            else if (!strncmp(line, "input_turn=", 11)) { if (!strcmp(line + 11, "smooth")) g_ic_snap = 0.0f; else if (!strcmp(line + 11, "snap") && g_ic_snap <= 0.0f) g_ic_snap = 30.0f; }
            else if (!strncmp(line, "input_btn_", 10)) {
                static const char *const nm[5] = { "a=", "b=", "bumper=", "x=", "y=" };
                static const char *const act[] = { "none", "reload", "jump", "duck", "attack2", "use", "attack" };
                int bi = -1;
                for (int i = 0; i < 5; i++) if (!strncmp(line + 10, nm[i], strlen(nm[i]))) bi = i;
                if (bi >= 0) {
                    const char *v = strchr(line + 10, '=') + 1;
                    int ai = -1;
                    for (int i = 0; i < 7; i++) if (!strcmp(v, act[i])) ai = i;
                    if (ai >= 0) g_ic_act[bi] = ai; else bmvr_log("input: bad %s (use none|reload|jump|duck|attack2|use|attack)", line);
                } else bmvr_log("input: unknown button in '%s' (use input_btn_a|b|bumper|x|y=)", line);
            }
            else if (!strncmp(line, "input_probe=", 12)) g_ic_probe = line[12] != '0';
            else if (!strncmp(line, "input_slot=", 11)) {
                long v = strtol(line + 11, NULL, 10);
                if (v >= 0 && v < 256) g_ic_slot = (int)v;
                else bmvr_log("input: bad input_slot=%s (use 0..255); keeping %d", line + 11, g_ic_slot);
            }
            else if (!strncmp(line, "input_turn_speed=", 17)) { float v = (float)atof(line + 17); if (v >= 10.0f && v <= 720.0f) g_ic_turn_speed = v; }
            else if (!strncmp(line, "input_snap=", 11)) { float v = (float)atof(line + 11); if (v == 0.0f || (v >= 5.0f && v <= 120.0f)) g_ic_snap = v; }
            else if (!strncmp(line, "every=", 6)) {
                long v = strtol(line + 6, NULL, 10);
                if (v >= 1 && v <= 3600) { g_x_every = (unsigned)v; bmvr_log("xfer: every=%ld (one delivery per %ld present(s))", v, v); }
                else bmvr_log("xfer: bad every=%s (use 1..3600); keeping %u", line + 6, g_x_every);
            }
            else if (first) parse_spec(&g_spec[0], line);
            first = 0;
        }
        if (f) fclose(f);
    }
    const char *e = getenv("BMVR_TARGET");
    if (e && *e) parse_spec(&g_spec[0], e);
    e = getenv("BMVR_TARGET2");
    if (e && *e) parse_spec2(e);
    e = getenv("BMVR_FLIP");
    if (e && *e) parse_flip(e);
    e = getenv("BMVR_FLIP2");
    if (e && *e) parse_flip2(e);
    e = getenv("BMVR_SYNC");
    if (e && *e) parse_sync(e);
    e = getenv("BMVR_STEREO");
    if (e && *e) g_st_want = e[0] == '1';
    e = getenv("BMVR_INPUT");
    if (e && *e) g_ic_want = e[0] == '1';
    if (g_ic_want) bmvr_log("input: requested (module '%s', class '%s', CreateMove slot %d, %s turning); it only watches until the slot proves to be CreateMove", g_ic_module, g_ic_class, g_ic_slot, g_ic_snap > 0.0f ? "snap" : "smooth");
    /* Stereo draws both eyes into the SAME engine target one after the other, so eye 1 needs no target2 of its own: it is the
     * same spec as eye 0. That also keeps the two-image negotiation (and the loading-screen fallback, which delivers a flat pair)
     * working exactly as with an explicit target2. */
    if (g_st_want && g_spec[0].kind != SPEC_WINDOW && !g_spec2_on) {
        g_spec[1] = g_spec[0];
        g_spec2_on = 1;
        bmvr_log("stereo: requested (module '%s', class '%s', RenderView slot %d, %d units/m); eye 1 reads the same target as eye 0", g_st_module, g_st_class, g_st_slot, (int)g_st_scale);
    } else if (g_st_want) {
        bmvr_log("stereo: requested but %s; stereo is off", g_st_want && g_spec[0].kind == SPEC_WINDOW ? "the target is the window (stereo needs a render target: set target=)" : "target2 is configured (stereo draws both eyes into one target; remove target2)");
        g_st_want = 0;
    }
    bmvr_log("xfer: sync mode requested: %s", g_sync_want ? "GPU semaphores (falls back to glFinish + ack if unavailable)" : "glFinish + ack");
    if (g_spec[0].kind == SPEC_NAME) bmvr_log("xfer: target = render target with label containing '%s', flip=%s", g_spec[0].name,
                                              g_flip_mode == 2 ? "alt" : (g_flip_mode ? "1" : "0"));
    else if (g_spec[0].kind == SPEC_SIZE) bmvr_log("xfer: target = any render target of GL size %ux%u, flip=%s", g_spec[0].w, g_spec[0].h,
                                                   g_flip_mode == 2 ? "alt" : (g_flip_mode ? "1" : "0"));
    else bmvr_log("xfer: target = window (default framebuffer, stage 2a behaviour)");
    if (g_spec2_on && g_spec[0].kind == SPEC_WINDOW) {
        bmvr_log("xfer: target2 ignored: eye 0 is the window, a pair needs two render targets");
        g_spec2_on = 0;
    }
    if (g_spec2_on) {
        int f2 = g_flip2_mode < 0 ? g_flip_mode : g_flip2_mode;
        if (g_spec[1].kind == SPEC_NAME) bmvr_log("xfer: target2 (eye 1) = render target with label containing '%s', flip=%s", g_spec[1].name,
                                                  f2 == 2 ? "alt" : (f2 ? "1" : "0"));
        else bmvr_log("xfer: target2 (eye 1) = any render target of GL size %ux%u, flip=%s", g_spec[1].w, g_spec[1].h,
                      f2 == 2 ? "alt" : (f2 ? "1" : "0"));
    }
}

/* ------------------------------------------------------------ render target registry */
/* Keyed by the address of the D3D object (texture or surface). Written from CreateTexture /
 * CreateRenderTarget / GetSurfaceLevel, read from SetRenderTarget.
 *
 * TWO tables, because the two kinds of entry live very differently:
 *   - pinned:    render-target textures and CreateRenderTarget surfaces. Created once, live for a level or
 *                the whole session, and everything else is looked up through them (a GetSurfaceLevel
 *                surface gets its label from its texture's entry). Only other pinned entries can displace
 *                one, the oldest first.
 *   - transient: the surface objects GetSurfaceLevel hands out. ToGL returns a fresh object nearly every
 *                call (~130 per frame in the real game) and nothing tells us when one dies, so this table
 *                is a cache that overwrites at will.
 * A single shared table (stage 2b) lost the long-lived labels within seconds once the churn filled it:
 * a pinned entry was evicted at random, the target was never found again (seen in the first real run,
 * and as a ~4% flake in the stand-in tests).
 * Entries are only ever overwritten (never emptied), so probe chains stay valid. */
#define REG_SIZE 4096
#define REG_PROBE 48
#define PIN_SIZE 1024
#define PIN_PROBE 64
#define PIN_NONE 0xFFFFFFFFu
#define PIN_TOMB ((void *)1)   /* a dead pinned entry; keeps probe chains walkable */
struct reg_ent {
    void *key;
    uint32_t w, h;
    int fmt;
    unsigned usage;
    unsigned seq;                 /* pinned table: insertion order, to evict the oldest */
    char label[40];
};
static struct reg_ent g_reg[REG_SIZE];       /* transient */
static struct reg_ent g_pin[PIN_SIZE];       /* pinned */
static volatile int g_reg_lock;
static unsigned g_reg_puts, g_reg_surf_puts, g_getsurf_total;
static unsigned g_pin_puts, g_pin_evicted, g_pin_seq;

static void reg_lock(void) { while (__sync_lock_test_and_set(&g_reg_lock, 1)) { /* spin: held for a few stores */ } }
static void reg_unlock(void) { __sync_lock_release(&g_reg_lock); }
static unsigned reg_hash_n(const void *p, unsigned size)
{
    uintptr_t v = (uintptr_t)p;
    v ^= v >> 12;
    v *= 2654435761u;
    return (unsigned)(v >> 8) & (size - 1);
}
#define reg_hash(p) reg_hash_n((p), REG_SIZE)

/* Pinned entry for the same address is stale once a new object is registered there (the old one died). */
static void pin_forget(const void *key)
{
    unsigned i = reg_hash_n(key, PIN_SIZE);
    for (unsigned n = 0; n < PIN_PROBE; n++, i = (i + 1) & (PIN_SIZE - 1)) {
        if (!g_pin[i].key) break;
        if (g_pin[i].key == key) { g_pin[i].key = PIN_TOMB; return; }   /* tombstone: keeps the chain walkable */
    }
}

/* Long-lived entry (render-target texture, CreateRenderTarget surface). */
static void reg_put_pinned(void *key, uint32_t w, uint32_t h, int fmt, unsigned usage, const char *label)
{
    if (!key) return;
    reg_lock();
    unsigned i = reg_hash_n(key, PIN_SIZE), found = PIN_NONE, spare = PIN_NONE, oldest = PIN_NONE;
    for (unsigned n = 0; n < PIN_PROBE; n++, i = (i + 1) & (PIN_SIZE - 1)) {
        struct reg_ent *c = &g_pin[i];
        if (!c->key) { if (spare == PIN_NONE) spare = i; break; }                 /* end of the chain: key is not in it */
        if (c->key == key) { found = i; break; }                                   /* same object registered again */
        if (c->key == PIN_TOMB) { if (spare == PIN_NONE) spare = i; continue; }  /* dead entry: reusable, but keep looking for the key */
        if (oldest == PIN_NONE || c->seq < g_pin[oldest].seq) oldest = i;
    }
    unsigned slot = found != PIN_NONE ? found : (spare != PIN_NONE ? spare : oldest);
    if (found == PIN_NONE && spare == PIN_NONE) g_pin_evicted++;                  /* chain full of live pinned entries: oldest goes */
    struct reg_ent *e = &g_pin[slot];
    e->key = key; e->w = w; e->h = h; e->fmt = fmt; e->usage = usage; e->seq = ++g_pin_seq;
    snprintf(e->label, sizeof e->label, "%s", label ? label : "");
    g_pin_puts++;
    reg_unlock();
}

static void reg_put(void *key, uint32_t w, uint32_t h, int fmt, unsigned usage, const char *label, int is_surface)
{
    if (!key) return;
    unsigned i = reg_hash(key), slot = i;
    reg_lock();
    pin_forget(key);
    for (unsigned n = 0; n < REG_PROBE; n++, i = (i + 1) & (REG_SIZE - 1)) {
        if (!g_reg[i].key || g_reg[i].key == key) { slot = i; break; }
        if (n == REG_PROBE - 1) slot = reg_hash(key); /* chain full: evict the home slot */
    }
    struct reg_ent *e = &g_reg[slot];
    e->key = key; e->w = w; e->h = h; e->fmt = fmt; e->usage = usage;
    snprintf(e->label, sizeof e->label, "%s", label ? label : "");
    g_reg_puts++;
    if (is_surface) g_reg_surf_puts++;
    reg_unlock();
}

static int reg_get(const void *key, struct reg_ent *out)
{
    if (!key) return 0;
    unsigned i = reg_hash_n(key, PIN_SIZE);
    int found = 0;
    reg_lock();
    for (unsigned n = 0; n < PIN_PROBE && !found; n++, i = (i + 1) & (PIN_SIZE - 1)) {
        if (!g_pin[i].key) break;
        if (g_pin[i].key == key) { *out = g_pin[i]; found = 1; }
    }
    if (found) { reg_unlock(); return 1; }
    i = reg_hash(key);
    for (unsigned n = 0; n < REG_PROBE; n++, i = (i + 1) & (REG_SIZE - 1)) {
        if (!g_reg[i].key) break;
        if (g_reg[i].key == key) { *out = g_reg[i]; found = 1; break; }
    }
    reg_unlock();
    return found;
}

/* ------------------------------------------------------------ GL helpers */
static int xfer_resolve_gl(void)
{
    static int tried, ok;
    if (tried) return ok;
    tried = 1;
    PFN_SDL_GL_GetProcAddress gp = g_getproc;
    if (!gp) { tried = 0; return 0; }
#define R(field, name) xgl.field = (void *)gp(name); if (!xgl.field) { bmvr_log("xfer: GL entry point %s missing", name); return 0; }
    R(GenTextures, "glGenTextures") R(DeleteTextures, "glDeleteTextures")
    R(BindTexture, "glBindTexture") R(TexParameteri, "glTexParameteri") R(GenFramebuffers, "glGenFramebuffers")
    R(DeleteFramebuffers, "glDeleteFramebuffers") R(BindFramebuffer, "glBindFramebuffer")
    R(FramebufferTexture2D, "glFramebufferTexture2D") R(FramebufferRenderbuffer, "glFramebufferRenderbuffer")
    R(CheckFramebufferStatus, "glCheckFramebufferStatus")
    R(BlitFramebuffer, "glBlitFramebuffer") R(Finish, "glFinish") R(Flush, "glFlush") R(GetError, "glGetError") R(IsEnabled, "glIsEnabled")
    R(Enable, "glEnable") R(Disable, "glDisable") R(GetIntegerv, "glGetIntegerv")
    R(GetFramebufferAttachmentParameteriv, "glGetFramebufferAttachmentParameteriv")
    R(GetTexLevelParameteriv, "glGetTexLevelParameteriv") R(BindRenderbuffer, "glBindRenderbuffer")
    R(GetRenderbufferParameteriv, "glGetRenderbufferParameteriv")
    /* The memory-object entry points are only needed for the helper hand-off; tracing works without them. */
    xgl.CreateMemoryObjects = (void *)gp("glCreateMemoryObjectsEXT");
    xgl.DeleteMemoryObjects = (void *)gp("glDeleteMemoryObjectsEXT");
    xgl.MemoryObjectParameteriv = (void *)gp("glMemoryObjectParameterivEXT");
    xgl.ImportMemoryFd = (void *)gp("glImportMemoryFdEXT");
    xgl.TexStorageMem2D = (void *)gp("glTexStorageMem2DEXT");
    xgl.IsTexture = (void *)gp("glIsTexture");
    xgl.IsRenderbuffer = (void *)gp("glIsRenderbuffer");
    xgl.GenSemaphores = (void *)gp("glGenSemaphoresEXT");
    xgl.DeleteSemaphores = (void *)gp("glDeleteSemaphoresEXT");
    xgl.ImportSemaphoreFd = (void *)gp("glImportSemaphoreFdEXT");
    xgl.WaitSemaphore = (void *)gp("glWaitSemaphoreEXT");
    xgl.SignalSemaphore = (void *)gp("glSignalSemaphoreEXT");
#undef R
    ok = 1;
    return 1;
}

static int xfer_have_sem(void)
{
    return xgl.GenSemaphores && xgl.DeleteSemaphores && xgl.ImportSemaphoreFd && xgl.WaitSemaphore && xgl.SignalSemaphore;
}

static int xfer_have_import(void)
{
    return xgl.CreateMemoryObjects && xgl.DeleteMemoryObjects && xgl.MemoryObjectParameteriv && xgl.ImportMemoryFd && xgl.TexStorageMem2D;
}

/* True when the calling thread has a current GL context (SDL's answer) and the entry points are loaded. */
static int xfer_gl_usable(void)
{
    return g_gl_reported && xfer_resolve_gl() && g_getctx_fn && g_getctx_fn();
}

static void xfer_teardown(void)
{
    if (g_x_sock >= 0) { close(g_x_sock); g_x_sock = -1; }
    for (int e = 0; e < XFER_EYES; e++) {
        if (xgl.DeleteFramebuffers && g_x_fbo[e]) xgl.DeleteFramebuffers(1, &g_x_fbo[e]);
        if (xgl.DeleteTextures && g_x_tex[e]) xgl.DeleteTextures(1, &g_x_tex[e]);
    }
    if (xgl.DeleteFramebuffers && g_x_rfbo) xgl.DeleteFramebuffers(1, &g_x_rfbo);
    if (xgl.DeleteSemaphores && g_x_sem_gl_ready) xgl.DeleteSemaphores(1, &g_x_sem_gl_ready);
    if (xgl.DeleteSemaphores && g_x_sem_vk_done) xgl.DeleteSemaphores(1, &g_x_sem_vk_done);
    for (int e = 0; e < XFER_EYES; e++)
        if (xgl.DeleteMemoryObjects && g_x_mem[e]) xgl.DeleteMemoryObjects(1, &g_x_mem[e]);
    g_x_sem_gl_ready = g_x_sem_vk_done = 0;
    g_x_sem_on = g_x_sem_inflight = 0;
    g_x_poses_on = 0;
    for (int e = 0; e < XFER_EYES; e++) g_x_fbo[e] = g_x_tex[e] = g_x_mem[e] = 0;
    g_x_rfbo = 0;
    g_x_neyes = 1;
}

static void xfer_set_timeout(int fd)
{
    struct timeval tv;
    tv.tv_sec = XFER_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* GL errors are drained and logged (first few only) so a bad call shows up in the log. */
static int xfer_gl_ok(const char *what)
{
    unsigned e = xgl.GetError();
    if (!e) return 1;
    static unsigned logged;
    if (logged++ < 8) bmvr_log("xfer: GL error 0x%x after %s", e, what);
    while (xgl.GetError()) { /* drain */ }
    return 0;
}

/* Receive hello + fd from the helper and import the memory as a GL texture. */
static int xfer_connect_and_import(void)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path) + 400];
    char dir[400];
    if (!game_dir(dir, sizeof dir)) return 0;
    snprintf(path, sizeof path, "%s/%s", dir, BMVR_XFER_SOCKET_NAME);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) {
        bmvr_log("xfer: socket path too long for a unix socket (%u chars): %s", (unsigned)strlen(path), path);
        g_x_state = -1;
        return 0;
    }
    strcpy(sa.sun_path, path);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return 0;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        static int told;
        if (!told++) bmvr_log("xfer: helper not running (no %s); will keep checking quietly", path);
        close(s);
        return 0;
    }
    xfer_set_timeout(s);

    struct bmvr_xfer_hello h;
    struct iovec iov;
    iov.iov_base = &h;
    iov.iov_len = sizeof h;
    char ctl[CMSG_SPACE(4 * sizeof(int))];    /* v4: up to 2 memory fds + 2 semaphore fds */
    memset(ctl, 0, sizeof ctl);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = sizeof ctl;
    ssize_t got = recvmsg(s, &m, 0);
    int fds[4] = { -1, -1, -1, -1 }, nfds = 0;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
        nfds = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        if (nfds > 4) nfds = 4;
        memcpy(fds, CMSG_DATA(c), (size_t)nfds * sizeof(int));
    }
    int fd = fds[0];
    int want_sem_fds = (got == (ssize_t)sizeof h && (h.caps & BMVR_XFER_CAP_SEMAPHORES)) ? 2 : 0;
    int offered = got == (ssize_t)sizeof h ? (int)h.eye_count : 0;
    if (got != (ssize_t)sizeof h || fd < 0 || h.magic != BMVR_XFER_MAGIC_HELLO || h.version != BMVR_XFER_VERSION ||
        !h.width || !h.height || h.width > 8192 || h.height > 8192 || offered < 1 || offered > XFER_EYES ||
        nfds != offered + want_sem_fds) {
        bmvr_log("xfer: bad hello from helper (got %d bytes, %d fd(s), version %u, want %u): rebuild shim and helper together",
                 (int)got, nfds, got == (ssize_t)sizeof h ? h.version : 0u, BMVR_XFER_VERSION);
        for (int i = 0; i < nfds; i++) close(fds[i]);
        close(s);
        return 0;
    }
    g_x_poses_on = (h.caps & BMVR_XFER_CAP_POSES) != 0;   /* v5 */
    memset(&g_x_pose_latest, 0, sizeof g_x_pose_latest);
    g_x_pose_pending = g_x_pose_used = 0;
    /* Eye 1 is used only when asked for (target2) and offered; an unused second memory fd is closed. */
    int neyes = (offered >= 2 && g_spec2_on && g_spec[0].kind != SPEC_WINDOW) ? 2 : 1;
    bmvr_log("xfer: helper connected: shared image %ux%u, memory %llu bytes, fd %d%s", h.width, h.height,
             (unsigned long long)h.memory_size, fd, offered > 1 ? " (+1 more image, eye 1)" : "");
    if (g_spec2_on && offered < 2)
        bmvr_log("xfer: target2 is configured but the helper serves one eye; delivering eye 0 only (start the helper with --eyes 2)");
    else if (offered >= 2 && !g_spec2_on)
        bmvr_log("xfer: the helper offers two eyes but no target2 is configured; delivering eye 0 only");
    for (int e = neyes; e < offered; e++) { close(fds[e]); fds[e] = -1; }

    if (!xfer_resolve_gl() || !xfer_have_import()) {
        bmvr_log("xfer: GL memory-object entry points missing; hand-off disabled");
        for (int i = 0; i < nfds; i++) close(fds[i]);
        close(s); g_x_state = -1; return 0;
    }
    while (xgl.GetError()) { /* drain errors left by the game */ }

    int old_tex = 0;
    xgl.GetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);

    int one = 1, ok = 1;
    for (int e = 0; e < neyes; e++) {
        if (!ok) { close(fds[e]); fds[e] = -1; continue; }      /* an earlier eye failed: this fd was not consumed */
        xgl.CreateMemoryObjects(1, &g_x_mem[e]);
        ok &= xfer_gl_ok("glCreateMemoryObjectsEXT");
        if (h.dedicated) { xgl.MemoryObjectParameteriv(g_x_mem[e], GL_DEDICATED_MEMORY_OBJECT_EXT, &one); ok &= xfer_gl_ok("dedicated flag"); }
        xgl.ImportMemoryFd(g_x_mem[e], h.memory_size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fds[e]); /* on success GL owns the fd */
        int imp = xfer_gl_ok("glImportMemoryFdEXT");
        ok &= imp;
        if (!imp) close(fds[e]);
        fds[e] = -1;
        xgl.GenTextures(1, &g_x_tex[e]);
        xgl.BindTexture(GL_TEXTURE_2D, g_x_tex[e]);
        if (h.optimal_tiling) xgl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, (int)GL_OPTIMAL_TILING_EXT);
        xgl.TexStorageMem2D(GL_TEXTURE_2D, 1, h.gl_internal_format, (int)h.width, (int)h.height, g_x_mem[e], 0);
        ok &= xfer_gl_ok("glTexStorageMem2DEXT");
        xgl.BindTexture(GL_TEXTURE_2D, (unsigned)old_tex);
    }

    if (ok) {
        int old_draw = 0, old_read = 0;
        xgl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
        xgl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
        xgl.GenFramebuffers(1, &g_x_rfbo);
        for (int e = 0; e < neyes; e++) {
            xgl.GenFramebuffers(1, &g_x_fbo[e]);
            xgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_x_fbo[e]);
            xgl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_x_tex[e], 0);
            unsigned st = xgl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE) { bmvr_log("xfer: framebuffer on the imported texture%s is incomplete (0x%x)", e ? " of eye 1" : "", st); ok = 0; }
        }
        xgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)old_draw);
        xgl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)old_read);
        ok &= xfer_gl_ok("framebuffer setup");
    }
    /* The semaphore fds follow the memory fds in the hello. */
    int sem_fd_gl = want_sem_fds ? fds[offered] : -1, sem_fd_vk = want_sem_fds ? fds[offered + 1] : -1;
    if (!ok) {
        bmvr_log("xfer: import failed; hand-off disabled for this session");
        if (want_sem_fds) { close(sem_fd_gl); close(sem_fd_vk); }
        xfer_teardown();
        close(s);
        g_x_state = -1;
        return 0;
    }
    /* Stage 3a: GPU semaphores, only when asked for (sync=sem), offered by the helper and present in GL. */
    if (want_sem_fds) {
        const char *why = NULL;
        if (!g_sync_want) why = "not requested (sync=finish)";
        else if (!xfer_have_sem()) why = "GL_EXT_semaphore_fd entry points missing";
        if (!why) {
            while (xgl.GetError()) { /* drain */ }
            xgl.GenSemaphores(1, &g_x_sem_gl_ready);
            xgl.GenSemaphores(1, &g_x_sem_vk_done);
            xgl.ImportSemaphoreFd(g_x_sem_gl_ready, GL_HANDLE_TYPE_OPAQUE_FD_EXT, sem_fd_gl);
            int ok1 = xfer_gl_ok("glImportSemaphoreFdEXT (gl_ready)");
            if (!ok1) close(sem_fd_gl);
            xgl.ImportSemaphoreFd(g_x_sem_vk_done, GL_HANDLE_TYPE_OPAQUE_FD_EXT, sem_fd_vk);
            int ok2 = xfer_gl_ok("glImportSemaphoreFdEXT (vk_done)");
            if (!ok2) close(sem_fd_vk);
            if (ok1 && ok2) { g_x_sem_on = 1; g_x_sem_inflight = 0; }
            else {
                why = "the driver rejected the semaphore import";
                xgl.DeleteSemaphores(1, &g_x_sem_gl_ready);
                xgl.DeleteSemaphores(1, &g_x_sem_vk_done);
                g_x_sem_gl_ready = g_x_sem_vk_done = 0;
            }
        } else {
            close(sem_fd_gl);
            close(sem_fd_vk);
        }
        if (g_x_sem_on) bmvr_log("xfer: sync = GPU semaphores (GL semaphores %u/%u imported); no glFinish, no CPU stall per frame", g_x_sem_gl_ready, g_x_sem_vk_done);
        else bmvr_log("xfer: sync = glFinish + ack (semaphores offered by the helper but %s)", why);
    } else {
        bmvr_log("xfer: sync = glFinish + ack%s", g_sync_want ? " (sync=sem was requested but the helper offered no semaphores: start it without --no-semaphores, on a driver with VK_KHR_external_semaphore_fd)" : "");
    }
    g_x_w = h.width;
    g_x_h = h.height;
    g_x_neyes = (unsigned)neyes;
    g_x_sock = s;
    g_x_state = 1;
    if (neyes == 2) bmvr_log("xfer: imported the helper's Vulkan memory as GL textures %u (eye 0) and %u (eye 1); pairs delivered every %u presents",
                             g_x_tex[0], g_x_tex[1], g_x_every);
    else bmvr_log("xfer: imported the helper's Vulkan memory as GL texture %u; snapshot every %u presents", g_x_tex[0], g_x_every);
    return 1;
}

/* ------------------------------------------------------------ what is attached to the draw FBO */
struct glatt {
    int fbo;            /* GL_DRAW_FRAMEBUFFER_BINDING */
    int type;           /* GL_TEXTURE_OBJ, GL_RENDERBUFFER, or 0 (none / default framebuffer) */
    unsigned name;
    int w, h, samples;
    int valid;          /* type known, size measured, single-sample */
};

/* Size and sample count of a GL texture / renderbuffer by name. Restores the binding it uses; w and h stay 0
 * when the object does not exist, is not 2D or the query fails. */
static void measure_gl_object(int type, unsigned name, int *pw, int *ph, int *psamples)
{
    int w = 0, h = 0, samples = 0;
    if (type == GL_TEXTURE_OBJ) {
        int old = 0;
        xgl.GetIntegerv(GL_TEXTURE_BINDING_2D, &old);
        xgl.BindTexture(GL_TEXTURE_2D, name);
        xgl.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        xgl.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        xgl.BindTexture(GL_TEXTURE_2D, (unsigned)old);   /* a multisample/array texture fails the bind: w stays 0 */
    } else {
        int old = 0;
        xgl.GetIntegerv(GL_RENDERBUFFER_BINDING, &old);
        xgl.BindRenderbuffer(GL_RENDERBUFFER, name);
        xgl.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &w);
        xgl.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h);
        xgl.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
        xgl.BindRenderbuffer(GL_RENDERBUFFER, (unsigned)old);
    }
    while (xgl.GetError()) { w = h = 0; }
    *pw = w; *ph = h; *psamples = samples;
}

/* Query the colour attachment 0 of the current DRAW framebuffer. Binds it to READ temporarily and
 * restores everything it touches. Caches the measurement of the last (type, name) for the rest of
 * the current Present only: GL recycles object names, so an older measurement can be wrong. */
static void probe_draw_fbo(struct glatt *a)
{
    static struct glatt cache;
    static unsigned cache_tick;
    memset(a, 0, sizeof *a);
    int fbo = 0;
    xgl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    a->fbo = fbo;
    if (!fbo) return;                       /* default framebuffer: not a render target */
    while (xgl.GetError()) { /* drain */ }
    int old_read = 0, type = 0, name = 0;
    xgl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    xgl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)fbo);
    xgl.GetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    xgl.GetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    xgl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)old_read);
    if (xgl.GetError()) { while (xgl.GetError()) { /* drain */ } return; }
    if (!name || (type != GL_TEXTURE_OBJ && type != GL_RENDERBUFFER)) return;
    a->type = type;
    a->name = (unsigned)name;
    if (cache.valid && cache_tick == g_x_tick && cache.type == type && cache.name == (unsigned)name) {
        a->w = cache.w; a->h = cache.h; a->samples = cache.samples; a->valid = 1;
        return;
    }
    int w = 0, h = 0, samples = 0;
    measure_gl_object(type, (unsigned)name, &w, &h, &samples);
    a->w = w; a->h = h; a->samples = samples;
    a->valid = (w > 0 && h > 0 && samples == 0);
    if (a->valid) { cache = *a; cache_tick = g_x_tick; }
}

/* ------------------------------------------------------------ SetRenderTarget(0) leave logic */
static void *volatile g_cur_rt0;         /* what the engine last bound as render target 0 */
static volatile int g_x_armed;           /* record the target the engine leaves (frame before a snapshot) */
static volatile int g_trace_left;        /* >0: log SetRenderTarget lines (one traced frame) */
static int g_trace_active;               /* a traced frame is in progress (ends at the next Present) */
static unsigned g_trace_frames_done;
static unsigned g_cap_leaves, g_cap_mismatch, g_cap_unresolved;
static unsigned g_setrt0_frame, g_setrt0_prev;   /* SetRenderTarget(0) calls in the current / previous frame */
static unsigned g_trace_next_tick;               /* earliest tick for the next trace */
static unsigned g_trace_events;                  /* events traced in the current traced frame */
static char g_trace_key[240];                    /* the pending (not yet printed) trace line, without pointers */
static unsigned g_trace_run;                     /* how many identical events it stands for */
static unsigned g_trace_printed;                 /* sequence lines printed in this traced frame */
#define TRACE_SEQ_LINES 150                      /* the sequence is cut here; the summary below it is complete */
#define TRACE_DISTINCT 96
static struct { char key[240]; unsigned count; } g_trace_tab[TRACE_DISTINCT];
static unsigned g_trace_ntab, g_trace_tab_overflow;

static void trace_flush(void)
{
    if (!g_trace_run) return;
    if (g_trace_printed < TRACE_SEQ_LINES) {
        g_trace_printed++;
        if (g_trace_run > 1) bmvr_log("rt-trace: x%-4u %s", g_trace_run, g_trace_key);
        else bmvr_log("rt-trace:       %s", g_trace_key);
        if (g_trace_printed == TRACE_SEQ_LINES) bmvr_log("rt-trace: (sequence cut after %d lines; the summary below is complete)", TRACE_SEQ_LINES);
    }
    g_trace_run = 0;
}

static void trace_count_distinct(const char *key)
{
    for (unsigned i = 0; i < g_trace_ntab; i++)
        if (!strcmp(g_trace_tab[i].key, key)) { g_trace_tab[i].count++; return; }
    if (g_trace_ntab < TRACE_DISTINCT) { memcpy(g_trace_tab[g_trace_ntab].key, key, sizeof g_trace_tab[0].key); g_trace_tab[g_trace_ntab++].count = 1; }
    else g_trace_tab_overflow++;
}

/* End of a traced frame: the sequence tells the ORDER (which target is left last before the back buffer),
 * the summary tells every distinct transition and how often it happened. */
static void trace_summary(void)
{
    bmvr_log("rt-trace: summary of the traced frame: %u distinct transitions%s", g_trace_ntab,
             g_trace_tab_overflow ? " (more existed than the table holds)" : "");
    for (unsigned i = 0; i < g_trace_ntab; i++) bmvr_log("rt-trace: sum x%-4u %s", g_trace_tab[i].count, g_trace_tab[i].key);
    g_trace_ntab = 0;
    g_trace_tab_overflow = 0;
}

struct captarget {
    int valid;
    int type;
    unsigned name, w, h;
    char label[40];
};
static struct captarget g_cap[XFER_EYES];          /* per eye: the last matching target left while armed */

/* The last target that was delivered. A loading screen / menu frame makes no SetRenderTarget(0) call (the engine
 * draws straight into the back buffer), so nothing is "left" and nothing is recorded; the last delivered object is
 * then re-checked and used instead. Not set by the window source. */
static struct captarget g_last_good[XFER_EYES];
static unsigned g_loading_fallbacks;

static int spec_matches(int eye, int known, const struct reg_ent *e, const struct glatt *a)
{
    const struct tspec *sp = &g_spec[eye];
    if (sp->kind == SPEC_SIZE) return a->valid && (unsigned)a->w == sp->w && (unsigned)a->h == sp->h;
    if (sp->kind != SPEC_NAME) return 0;
    return known && ci_contains(e->label, sp->name);
}

/* Called from the SetRenderTarget interposer BEFORE the real call, for index 0.
 * `left` is the surface that was bound until now, `next` the one about to be bound. */
static void rt_leave(void *left, void *next)
{
    int tracing = g_trace_left > 0;
    if (!tracing && !g_x_armed) return;
    if (!xfer_gl_usable()) return;

    struct reg_ent le, ne;
    int known = reg_get(left, &le), nknown = reg_get(next, &ne);
    struct glatt a;
    if (left) probe_draw_fbo(&a); else memset(&a, 0, sizeof a);

    if (tracing) {
        g_trace_left--;
        g_trace_events++;
        char key[sizeof g_trace_key];   /* no pointers in it: the engine hands out a fresh surface object nearly every call */
        snprintf(key, sizeof key, "left=[%s %ux%u] gl fbo=%d %s name=%u %dx%d samples=%d | next=[%s %ux%u]",
                 known ? le.label : "?", known ? le.w : 0u, known ? le.h : 0u, a.fbo,
                 a.type == GL_TEXTURE_OBJ ? "tex" : a.type == GL_RENDERBUFFER ? "rb" : "-", a.name, a.w, a.h, a.samples,
                 nknown ? ne.label : "?", nknown ? ne.w : 0u, nknown ? ne.h : 0u);
        trace_count_distinct(key);
        if (g_trace_run && !strcmp(key, g_trace_key)) g_trace_run++;
        else { trace_flush(); memcpy(g_trace_key, key, sizeof g_trace_key); g_trace_run = 1; }
    }
    if (!g_x_armed || !left || g_spec[0].kind == SPEC_WINDOW) return;

    g_cap_leaves++;
    if (!a.valid) { g_cap_unresolved++; return; }
    for (unsigned e = 0; e < g_x_neyes; e++) {   /* one leave can serve both eyes (same object for both) */
        if (!spec_matches((int)e, known, &le, &a)) continue;
        if (known && ((uint32_t)a.w != le.w || (uint32_t)a.h != le.h)) { g_cap_mismatch++; continue; } /* stale / lazy-bind guard */
        g_cap[e].valid = 1;
        g_cap[e].type = a.type;
        g_cap[e].name = a.name;
        g_cap[e].w = (unsigned)a.w;
        g_cap[e].h = (unsigned)a.h;
        snprintf(g_cap[e].label, sizeof g_cap[e].label, "%s", known && le.label[0] ? le.label : "size");
    }
}

/* ------------------------------------------------------------ delivering a frame */
static int xfer_pick_flip(int eye)
{
    int mode = (eye && g_flip2_mode >= 0) ? g_flip2_mode : g_flip_mode;
    if (mode == 0 || mode == 1) return mode;
    return (int)(g_x_snapshots & 1u);   /* "alt": both eyes of a pair use the same orientation */
}

/* Before writing the shared image: with semaphores, make the GPU wait until the helper finished reading the
 * previous frame (never before the first frame: nothing signals vk_done then). */
static void xfer_sync_before_blit(void)
{
    if (!g_x_sem_on || !g_x_sem_inflight) return;
    unsigned layout[XFER_EYES] = { GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT };
    xgl.WaitSemaphore(g_x_sem_vk_done, 0, NULL, g_x_neyes, g_x_tex, layout);   /* one wait covers both eyes' images */
    g_x_sem_inflight = 0;
    if (!xfer_gl_ok("glWaitSemaphoreEXT")) { g_x_sem_on = 0; bmvr_log("xfer: GL semaphore wait failed; using glFinish + ack from now on"); }
}

/* After the blit: publish the frame. Semaphores: signal gl_ready and flush (the GPU orders the rest).
 * Otherwise a full glFinish. Returns the frame flags to send. */
static unsigned xfer_sync_publish(void)
{
    if (g_x_sem_on) {
        unsigned layout[XFER_EYES] = { GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT };
        xgl.SignalSemaphore(g_x_sem_gl_ready, 0, NULL, g_x_neyes, g_x_tex, layout);   /* ONE signal per pair */
        xgl.Flush();
        if (xfer_gl_ok("glSignalSemaphoreEXT")) return BMVR_XFER_FLAG_SEMAPHORE;
        g_x_sem_on = 0;
        bmvr_log("xfer: GL semaphore signal failed; using glFinish + ack from now on");
    }
    xgl.Finish(); /* test path: the stall this stage replaces */
    return 0;
}

/* What was written into the shared image of one eye. */
struct eyeinfo {
    unsigned w, h, flags, gl_name;
    char name[40];
};

/* v6: controller state from the ack. Slice 2 (verification): log what arrives, change by change, so a headset run can confirm the path
 * before anything is fed to the game. bmvr_get_input() is the interface the game-side hook (slice 3) will use. */
static struct bmvr_xfer_input g_x_input;
static unsigned g_x_input_logs;
static unsigned in_quant(const struct bmvr_xfer_input *n)   /* a coarse state: buttons, trigger/squeeze > 0.5, stick directions at 0.5 */
{
    unsigned q = n->buttons & 0xffffu;
    for (int h = 0; h < 2; h++) {
        if (n->trigger[h] > 0.5f) q |= 1u << (16 + h * 8);
        if (n->squeeze[h] > 0.5f) q |= 2u << (16 + h * 8);
        if (n->stick[h][0] > 0.5f) q |= 4u << (16 + h * 8);
        if (n->stick[h][0] < -0.5f) q |= 8u << (16 + h * 8);
        if (n->stick[h][1] > 0.5f) q |= 16u << (16 + h * 8);
        if (n->stick[h][1] < -0.5f) q |= 32u << (16 + h * 8);
    }
    return q;
}
static void in_note(const struct bmvr_xfer_input *n)
{
    if (!n->seq) return;
    const struct bmvr_xfer_input old = g_x_input;
    g_x_input = *n;
    if (g_x_input_logs >= 200) return;
    if (!old.seq) {
        g_x_input_logs++;
        bmvr_log("input: first controller state (seq %u): focused=%d left pose %s right pose %s; right aim (%.2f %.2f %.2f) left aim (%.2f %.2f %.2f) [LOCAL space, metres]",
                 n->seq, !!(n->flags & BMVR_XFER_INPUT_FOCUSED), (n->flags & BMVR_XFER_INPUT_POSE_L) ? "valid" : "none", (n->flags & BMVR_XFER_INPUT_POSE_R) ? "valid" : "none",
                 (double)n->aim[1][0], (double)n->aim[1][1], (double)n->aim[1][2], (double)n->aim[0][0], (double)n->aim[0][1], (double)n->aim[0][2]);
        return;
    }
    if (in_quant(&old) != in_quant(n) || ((old.flags ^ n->flags) & (BMVR_XFER_INPUT_FOCUSED | BMVR_XFER_INPUT_POSE_L | BMVR_XFER_INPUT_POSE_R))) {
        g_x_input_logs++;
        bmvr_log("input: focused=%d poses L%d R%d | buttons L %#x R %#x | stick L (%.2f %.2f) R (%.2f %.2f) | trigger L %.2f R %.2f | squeeze L %.2f R %.2f",
                 !!(n->flags & BMVR_XFER_INPUT_FOCUSED), !!(n->flags & BMVR_XFER_INPUT_POSE_L), !!(n->flags & BMVR_XFER_INPUT_POSE_R), n->buttons & 0xffu, (n->buttons >> 8) & 0xffu,
                 (double)n->stick[0][0], (double)n->stick[0][1], (double)n->stick[1][0], (double)n->stick[1][1],
                 (double)n->trigger[0], (double)n->trigger[1], (double)n->squeeze[0], (double)n->squeeze[1]);
    }
}

/* v6 hook-port interface: the newest controller state. Returns 1 and fills *out, or 0 when the helper offers no input / none arrived yet. */
BMVR_EXPORT int bmvr_get_input(struct bmvr_xfer_input *out)
{
    if (!g_x_input.seq) return 0;
    *out = g_x_input;
    return 1;
}

/* Tell the helper the shared image(s) hold a frame, wait for its ack. Returns 1 on success.
 * `semflag` (BMVR_XFER_FLAG_SEMAPHORE or 0) applies to the whole pair; ei[0] is always valid, ei[1] when neyes == 2. */
static int xfer_send_frame(unsigned neyes, const struct eyeinfo *ei, unsigned semflag, uint32_t pose_id)
{
    unsigned sw = ei[0].w, sh = ei[0].h, flags = ei[0].flags | semflag;
    const char *name = ei[0].name;
    struct bmvr_xfer_frame fr;
    memset(&fr, 0, sizeof fr);
    fr.magic = BMVR_XFER_MAGIC_FRAME;
    fr.tick = g_x_tick;
    fr.src_w = sw;
    fr.src_h = sh;
    fr.flags = flags;
    fr.gl_name = ei[0].gl_name;
    snprintf(fr.name, sizeof fr.name, "%.23s", name);
    fr.eye_count = neyes;
    fr.pose_id = g_x_poses_on ? pose_id : 0;   /* v5: the pose this pair was rendered with */
    if (neyes == 2) {
        fr.eye1_src_w = ei[1].w;
        fr.eye1_src_h = ei[1].h;
        fr.eye1_flags = ei[1].flags;
        fr.eye1_gl_name = ei[1].gl_name;
        snprintf(fr.eye1_name, sizeof fr.eye1_name, "%.23s", ei[1].name);
    }
    uint64_t t1 = now_ns();
    struct bmvr_xfer_ack ack;
    if (send(g_x_sock, &fr, sizeof fr, MSG_NOSIGNAL) != (ssize_t)sizeof fr ||
        recv(g_x_sock, &ack, sizeof ack, MSG_WAITALL) != (ssize_t)sizeof ack || ack.magic != BMVR_XFER_MAGIC_ACK) {
        bmvr_log("xfer: lost the helper (no ack); back to searching");
        xfer_teardown();
        g_x_state = 0;
        return 0;
    }
    uint64_t t2 = now_ns();
    if (g_x_poses_on && ack.pose.pose_id) {
        if (!g_x_pose_latest.pose_id)
            bmvr_log("xfer: pose channel: first pose id %u (%s space), left eye at (%.3f %.3f %.3f) fov l/r/u/d %.3f %.3f %.3f %.3f rad", ack.pose.pose_id,
                     (ack.pose.flags & BMVR_XFER_POSE_LOCAL) ? "LOCAL" : "VIEW", (double)ack.pose.eye[0].pos[0], (double)ack.pose.eye[0].pos[1], (double)ack.pose.eye[0].pos[2],
                     (double)ack.pose.eye[0].fov[0], (double)ack.pose.eye[0].fov[1], (double)ack.pose.eye[0].fov[2], (double)ack.pose.eye[0].fov[3]);
        g_x_pose_latest = ack.pose;
    }
    in_note(&ack.input);   /* v6: controllers (seq 0 = none) */
    if (flags & BMVR_XFER_FLAG_SEMAPHORE) g_x_sem_inflight = 1;   /* the helper's vk_done signal is now pending */
    unsigned long long gl_us = g_x_t0 && t1 > g_x_t0 ? (unsigned long long)((t1 - g_x_t0) / 1000u) : 0ull;
    unsigned long long rtt_us = (unsigned long long)((t2 - t1) / 1000u);
    g_x_cost_gl_us += gl_us;
    g_x_cost_rtt_us += rtt_us;
    g_x_cost_n++;
    if (g_x_snapshots++ < 6) {
        char eye1[96] = "";
        if (neyes == 2) snprintf(eye1, sizeof eye1, "; eye 1 '%s' %ux%u flip=%u", ei[1].name, ei[1].w, ei[1].h, ei[1].flags & BMVR_XFER_FLAG_FLIPPED);
        bmvr_log("xfer: snapshot %u delivered (tick %u, '%s' %ux%u -> %ux%u, flip=%u, sync=%s, GL side %llu us, helper round trip %llu us%s)",
                 g_x_snapshots, g_x_tick, name, sw, sh, g_x_w, g_x_h, flags & BMVR_XFER_FLAG_FLIPPED,
                 (flags & BMVR_XFER_FLAG_SEMAPHORE) ? "semaphores" : "glFinish+ack", gl_us, rtt_us, eye1);
    }
    if (g_x_cost_n % 10 == 0 && g_x_cost_n <= 50)
        bmvr_log("xfer: average over %u snapshots (sync=%s): GL side %llu us, helper round trip %llu us",
                 g_x_cost_n, (flags & BMVR_XFER_FLAG_SEMAPHORE) ? "semaphores" : "glFinish+ack",
                 (unsigned long long)(g_x_cost_gl_us / g_x_cost_n), (unsigned long long)(g_x_cost_rtt_us / g_x_cost_n));
    return 1;
}

/* Stage 2a path: blit the window's default framebuffer (the frame about to be presented). */
static void xfer_snapshot_window(void)
{
    int vp[4] = {0, 0, 0, 0}, old_draw = 0, old_read = 0, read_buf = 0;
    xgl.GetIntegerv(GL_VIEWPORT, vp);
    xgl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    xgl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    xgl.GetIntegerv(GL_READ_BUFFER, &read_buf);
    int sw = vp[2], sh = vp[3];
    if (sw <= 0 || sh <= 0) return;
    if (g_x_snapshots == 0)
        bmvr_log("xfer: first snapshot: read fb=%d draw fb=%d read buffer=0x%x viewport=%dx%d (reading from the default framebuffer)",
                 old_read, old_draw, read_buf, sw, sh);

    unsigned char scissor = xgl.IsEnabled(GL_SCISSOR_TEST);
    if (scissor) xgl.Disable(GL_SCISSOR_TEST);
    while (xgl.GetError()) { /* drain */ }
    xgl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    xgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_x_fbo[0]);
    xfer_sync_before_blit();
    /* Destination Y range is flipped: GL rows start at the bottom, the Vulkan image's row 0 is the top. */
    xgl.BlitFramebuffer(0, 0, sw, sh, 0, (int)g_x_h, (int)g_x_w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    int blit_ok = xfer_gl_ok("glBlitFramebuffer");
    unsigned semflag = blit_ok ? xfer_sync_publish() : (xgl.Finish(), 0u);
    xgl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)old_read);
    xgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)old_draw);
    if (scissor) xgl.Enable(GL_SCISSOR_TEST);
    if (!blit_ok) return;
    struct eyeinfo ei[XFER_EYES];
    memset(ei, 0, sizeof ei);
    ei[0].w = (unsigned)sw; ei[0].h = (unsigned)sh; ei[0].flags = BMVR_XFER_FLAG_FLIPPED | BMVR_XFER_FLAG_WINDOW;
    snprintf(ei[0].name, sizeof ei[0].name, "%s", "window");
    xfer_send_frame(1, ei, semflag, g_st_want ? 0 : g_x_pose_used);
}

/* Loading screen / menu: the armed frame never switched render targets, so nothing was "left" for this eye. Reuse the
 * last delivered object of the eye if it still exists with the same size and is single-sample. Returns 1 when g_cap[e]
 * was filled from it. */
static int cap_use_fallback(unsigned e)
{
    if (g_cap[e].valid || g_setrt0_prev != 0 || !g_last_good[e].valid) return 0;
    struct captarget *lg = &g_last_good[e];
    int exists = 1, w = 0, h = 0, samples = 0;
    if (lg->type == GL_TEXTURE_OBJ && xgl.IsTexture) exists = xgl.IsTexture(lg->name) != 0;
    if (lg->type == GL_RENDERBUFFER && xgl.IsRenderbuffer) exists = xgl.IsRenderbuffer(lg->name) != 0;
    if (exists) measure_gl_object(lg->type, lg->name, &w, &h, &samples);
    if (exists && w == (int)lg->w && h == (int)lg->h && samples == 0) {
        g_cap[e] = *lg;
        if (g_loading_fallbacks++ < 3)
            bmvr_log("xfer: loading/menu frame (0 SetRenderTarget(0) calls): delivering the last known object of '%s' (%s %u, %ux%u)%s",
                     g_cap[e].label, g_cap[e].type == GL_TEXTURE_OBJ ? "texture" : "renderbuffer", g_cap[e].name, g_cap[e].w, g_cap[e].h,
                     e ? " [eye 1]" : "");
        return 1;
    }
    lg->valid = 0;   /* gone or resized: forget it, wait for the next gameplay frame */
    bmvr_log("xfer: the last known object of the target%s no longer exists or changed size; waiting for a gameplay frame", e ? " (eye 1)" : "");
    return 0;
}

/* GL state the blits disturb: saved before, restored after (the engine's bindings must survive: tests T1-T23 check it). */
struct glstate { int draw, read; unsigned char scissor; };
static void xfer_gl_save(struct glstate *g)
{
    g->draw = g->read = 0;
    xgl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &g->draw);
    xgl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &g->read);
    g->scissor = xgl.IsEnabled(GL_SCISSOR_TEST);
    if (g->scissor) xgl.Disable(GL_SCISSOR_TEST);
    while (xgl.GetError()) { /* drain */ }
}
static void xfer_gl_restore(const struct glstate *g)
{
    xgl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)g->read);
    xgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)g->draw);
    if (g->scissor) xgl.Enable(GL_SCISSOR_TEST);
    while (xgl.GetError()) { /* drain whatever the teardown raised */ }
}

/* Blit one captured target into the shared image of eye `e`. The caller saved the GL state. *synced: the wait for the helper's
 * previous read is issued once per pair, before the first blit. Returns 1 on success. */
static int xfer_blit_eye(unsigned e, const struct captarget *cap, int *synced, struct eyeinfo *ei)
{
    int ok = 1;
    int flip = xfer_pick_flip((int)e);
    xgl.BindFramebuffer(GL_READ_FRAMEBUFFER, g_x_rfbo);
    if (cap->type == GL_TEXTURE_OBJ) xgl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cap->name, 0);
    else xgl.FramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, cap->name);
    unsigned st = xgl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (st == GL_FRAMEBUFFER_COMPLETE && xfer_gl_ok("attach target")) {
        xgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_x_fbo[e]);
        if (!*synced) { xfer_sync_before_blit(); *synced = 1; }   /* ONE wait before the first blit of the pair */
        xgl.BlitFramebuffer(0, 0, (int)cap->w, (int)cap->h, 0, flip ? (int)g_x_h : 0, (int)g_x_w, flip ? 0 : (int)g_x_h,
                            GL_COLOR_BUFFER_BIT, GL_LINEAR);
        ok = xfer_gl_ok("glBlitFramebuffer (target)");
    } else {
        static unsigned told;
        if (told++ < 4) bmvr_log("xfer: read FBO on '%s' (%s %u) is not usable (status 0x%x) - object deleted or unsupported format?",
                                 cap->label, cap->type == GL_TEXTURE_OBJ ? "texture" : "renderbuffer", cap->name, st);
        while (xgl.GetError()) { /* drain */ }
        ok = 0;
    }
    /* Drop the attachment so the private FBO never keeps (or later aliases) the engine's object. */
    xgl.BindFramebuffer(GL_READ_FRAMEBUFFER, g_x_rfbo);
    xgl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    ei->w = cap->w; ei->h = cap->h; ei->gl_name = cap->name;
    ei->flags = flip ? BMVR_XFER_FLAG_FLIPPED : 0;
    snprintf(ei->name, sizeof ei->name, "%s", cap->label);
    return ok;
}

/* Stage 2b / 3d path: blit the render target(s) recorded by rt_leave(). With two eyes the pair is delivered only when
 * BOTH eyes were captured in the same armed frame (a half pair would mix two game frames), with one wait before the
 * first blit and one signal after the second. */
static void xfer_snapshot_target(void)
{
    const unsigned neyes = g_x_neyes;
    int from_fallback[XFER_EYES] = { 0, 0 };
    for (unsigned e = 0; e < neyes; e++) from_fallback[e] = cap_use_fallback(e);

    unsigned missing = 0;
    for (unsigned e = 0; e < neyes; e++) if (!g_cap[e].valid) missing |= 1u << e;
    if (missing) {
        static unsigned told, told1;
        if (!g_setrt0_prev) return;   /* a loading frame before any gameplay frame: nothing to deliver yet, nothing wrong */
        if (missing & 1u) {
            if (told++ < 6) {
                bmvr_log("xfer: no matching render target was left in the armed frame (SetRenderTarget(0) calls=%u, leaves seen=%u unresolved=%u size-mismatch=%u "
                         "registry puts=%u/%u, GetSurfaceLevel calls=%u) - see the rt-trace lines, adjust bmvr_target.txt",
                         g_setrt0_prev, g_cap_leaves, g_cap_unresolved, g_cap_mismatch, g_reg_puts, g_reg_surf_puts, g_getsurf_total);
                if (told == 1 && !g_getsurf_total && g_spec[0].kind == SPEC_NAME)
                    bmvr_log("xfer: hint: IDirect3DTexture9::GetSurfaceLevel was never seen, so surfaces have no labels "
                             "(libtogl may not export it). Select by size instead: BMVR_TARGET=size:WxH (sizes are in the rt-trace lines).");
            }
        } else if (told1++ < 6) {
            bmvr_log("xfer: no matching render target for eye 1 (target2) was left in the armed frame (SetRenderTarget(0) calls=%u, leaves seen=%u "
                     "unresolved=%u size-mismatch=%u); the pair is not delivered - see the rt-trace lines, adjust target2 in bmvr_target.txt",
                     g_setrt0_prev, g_cap_leaves, g_cap_unresolved, g_cap_mismatch);
        }
        for (unsigned e = 0; e < neyes; e++) g_cap[e].valid = 0;   /* drop a half pair */
        return;
    }
    struct captarget cap[XFER_EYES];
    for (unsigned e = 0; e < neyes; e++) { cap[e] = g_cap[e]; g_cap[e].valid = 0; }

    struct glstate gs;
    xfer_gl_save(&gs);

    int ok = 1, synced = 0;
    struct eyeinfo ei[XFER_EYES];
    memset(ei, 0, sizeof ei);
    for (unsigned e = 0; e < neyes && ok; e++) ok = xfer_blit_eye(e, &cap[e], &synced, &ei[e]);
    unsigned semflag = ok ? xfer_sync_publish() : 0u;   /* ONE signal (or one glFinish) for the whole pair */
    xfer_gl_restore(&gs);
    if (!ok) return;
    if (xfer_send_frame(neyes, ei, semflag, g_st_want ? 0 : g_x_pose_used)) {   /* stereo requested: a pair the hook did not draw carries no pose (it was not rendered with one) */
        for (unsigned e = 0; e < neyes; e++) {
            if (from_fallback[e]) continue;
            g_last_good[e] = cap[e];
            g_last_good[e].valid = 1;
        }
    }
}

static int xfer_peer_alive(void);

/* ====================================================================== stage 4b: stereo views
 * The engine renders ONE view per frame. With "stereo=1" the shim hooks the client's RenderView (a vtable slot found at run time
 * from RTTI, linux/bmvr_rtti.h), and for a delivery frame draws the main world view TWICE: once per eye, with the origin, angles
 * and off-centre frustum computed from the pose the helper sent (linux/bmvr_stereo.h). After each pass the target the engine
 * just drew is blitted into that eye's shared image; after the second the pair is published as one unit and the frame message
 * carries the pose id the pair was rendered with (bmvr_xfer.h v5). Frames that are not delivery frames, nested views (water,
 * monitors, cubemaps), small views and any frame without a usable pose are drawn by the game's own RenderView, untouched.
 *
 * STATUS: stand-in tested only (tests/linux, T24-T27). What nothing here has been checked against: the real client.so (is the
 * class found, is RenderView really slot 6, does CViewSetup have the layout below on Linux, is the target complete at the end of
 * the pass). Every one of those is validated at run time and logged, and a failed validation leaves the game untouched. */
#define ST_DRAW_HUD 0x2          /* RENDERVIEW_DRAWHUD (src/hooks.cpp kRenderViewDrawHud): the 2D HUD is not drawn into a 3D eye */
#define ST_VS_COPY 0x200         /* bytes copied for a per-eye CViewSetup (the Windows build's is 0x148; over-copying is harmless) */
/* CViewSetup offsets on the Windows BM client (src/sdk/sdk.h). Not assumed: st_view_sane() checks them on every view. */
#define VS_X 0x00
#define VS_Y 0x08
#define VS_W 0x10
#define VS_H 0x18
#define VS_FOV 0xB4
#define VS_FOVVM 0xB8
#define VS_ORIGIN 0xBC
#define VS_ANGLES 0xC8
#define VS_ZNEAR 0xD4
#define VS_ZFAR 0xD8
#define VS_ASPECT 0xE8
#define VS_OFFC 0xEC
#define VS_OC_TOP 0xF0
#define VS_OC_BOTTOM 0xF4
#define VS_OC_LEFT 0xF8
#define VS_OC_RIGHT 0xFC
#define ST_MAX_FAILS 5           /* consecutive failed eye captures before stereo switches itself off */
#define ST_FLAG_CLEAR_FRAMES 600 /* good stereo frames after which the crash-sticky flag is removed */

typedef void (*PFN_RenderView)(void *, void *, int, int);
static int g_st_state;                  /* 0 = not tried, 1 = hooked, -1 = refused / failed */
static PFN_RenderView g_st_orig;
static struct bmvr_stereo_rig g_st_rig;
static volatile int g_st_depth;         /* RenderView nesting on the thread that is inside the hook */
static long g_st_tid;
static int g_st_sent;                   /* this frame's pair was delivered from inside the hook */
static unsigned g_st_frames, g_st_fails, g_st_consec_fail, g_st_pass_main;
static int g_st_flag_on;
static char g_st_flag_path[512];
/* Diagnostics: the hook is stand-in tested only, so a real run must say WHERE it stops. */
static unsigned g_st_calls, g_st_nested, g_st_top, g_st_dumped;   /* detour entries / nested / top-level / raw view dumps written */
static unsigned g_st_silent[6];                                    /* silent exits: 0 not armed, 1 no GL context on this thread, 2 small view, 3 helper gone, 4 blit failed, 5 send failed */
static long g_st_ctx_tid;                                          /* thread that last drew a stereo pair */
static unsigned g_st_why_cnt[8];                                   /* per st_note() reason: how many views were let through */
static unsigned g_st_hook_tick;                                    /* g_x_tick when the vtable slot was patched */
static int g_st_never_warned;
static uintptr_t *g_st_vt_slot;                                    /* the patched vtable slot (NULL in inline-only mode) */
static uintptr_t g_st_vt_old;                                      /* what the slot held: the real RenderView entry */
static int g_st_inline_tried;                                      /* the inline detour was attempted (once) */
static unsigned g_st_next_try;                                     /* g_x_tick before which st_try_install does not look for the module again */

static float vs_f(const unsigned char *v, unsigned off) { float f; memcpy(&f, v + off, sizeof f); return f; }
static int vs_i(const unsigned char *v, unsigned off) { int i; memcpy(&i, v + off, sizeof i); return i; }
static void vs_setf(unsigned char *v, unsigned off, float f) { memcpy(v + off, &f, sizeof f); }
static int fin(float f) { return f == f && f > -1e7f && f < 1e7f; }

/* The layout check that stands in for "we know CViewSetup on Linux": every field we read or write must look like what it is. */
static int st_view_sane(const unsigned char *v)
{
    const int w = vs_i(v, VS_W), h = vs_i(v, VS_H);
    if (w < 640 || h < 360 || w > 16384 || h > 16384) return 0;
    if (vs_i(v, VS_X) != 0 || vs_i(v, VS_Y) != 0) return 0;          /* a sub-rectangle view is not the main view */
    const float fov = vs_f(v, VS_FOV), asp = vs_f(v, VS_ASPECT), zn = vs_f(v, VS_ZNEAR), zf = vs_f(v, VS_ZFAR);
    if (!fin(fov) || fov < 20.0f || fov > 170.0f) return 0;
    if (!fin(asp) || asp < 0.4f || asp > 4.0f) return 0;
    if (!fin(zn) || !fin(zf) || zn <= 0.0f || zf <= zn) return 0;
    for (unsigned i = 0; i < 3; i++) if (!fin(vs_f(v, VS_ORIGIN + 4 * i)) || !fin(vs_f(v, VS_ANGLES + 4 * i))) return 0;
    if (fabsf(vs_f(v, VS_ANGLES)) > 91.0f) return 0;
    return 1;
}

static void st_apply(unsigned char *v, const struct bmvr_view *e)
{
    for (unsigned i = 0; i < 3; i++) { vs_setf(v, VS_ORIGIN + 4 * i, e->origin[i]); vs_setf(v, VS_ANGLES + 4 * i, e->angles[i]); }
    vs_setf(v, VS_FOV, e->fov_deg);
    vs_setf(v, VS_FOVVM, e->fov_deg);
    vs_setf(v, VS_ASPECT, e->aspect);
    v[VS_OFFC] = 1;
    vs_setf(v, VS_OC_LEFT, e->frac[0]);
    vs_setf(v, VS_OC_RIGHT, e->frac[1]);
    vs_setf(v, VS_OC_BOTTOM, e->frac[2]);
    vs_setf(v, VS_OC_TOP, e->frac[3]);
}

/* Why the hook let a view through untouched: logged once per reason (a game frame has many RenderViews). */
static void st_note(unsigned reason, const char *why)
{
    static unsigned seen;
    if (reason < 8) g_st_why_cnt[reason]++;
    if (seen & (1u << reason)) return;
    seen |= 1u << reason;
    bmvr_log("stereo: drawing the game's own view: %s (logged once)", why);
}

/* The engine target that holds the eye that was just drawn: what rt_leave() saw it leave, else what is bound right now. */
static int st_find_target(struct captarget *cap)
{
    if (g_cap[0].valid) { *cap = g_cap[0]; return 1; }
    struct reg_ent re;
    memset(&re, 0, sizeof re);
    int known = g_cur_rt0 && reg_get(g_cur_rt0, &re);
    struct glatt a;
    probe_draw_fbo(&a);
    if (!a.valid || !spec_matches(0, known, &re, &a)) return 0;
    if (known && ((uint32_t)a.w != re.w || (uint32_t)a.h != re.h)) return 0;
    cap->valid = 1; cap->type = a.type; cap->name = a.name; cap->w = (unsigned)a.w; cap->h = (unsigned)a.h;
    snprintf(cap->label, sizeof cap->label, "%s", known && re.label[0] ? re.label : "size");
    return 1;
}

static void st_fail_eye(unsigned e)
{
    g_st_fails++;
    if (++g_st_consec_fail >= ST_MAX_FAILS) {
        g_st_state = -1;
        bmvr_log("stereo: eye %u: no matching render target after the pass, %u times in a row; stereo switched OFF (the game draws its own view). "
                 "Check target= in bmvr_target.txt against the rt-trace lines.", e, g_st_consec_fail);
    } else if (g_st_fails <= 3) {
        bmvr_log("stereo: eye %u: no render target matching the spec was left or is bound after the pass; the pair is dropped", e);
    }
}

/* The first top-level views, raw: if st_view_sane() refuses, this is what the real CViewSetup layout looks like. */
static void st_dump_view(const unsigned char *v)
{
    if (g_st_dumped >= 3) return;
    g_st_dumped++;
    bmvr_log("stereo: top-level RenderView #%u raw view (0x150 bytes, hex words, then the words that read as plausible floats):", g_st_dumped);
    for (unsigned off = 0; off < 0x150; off += 32) {
        char line[200];
        int n = snprintf(line, sizeof line, "stereo:   +%03x:", off);
        for (unsigned i = 0; i < 8 && off + 4 * i < 0x150; i++) {
            unsigned w; memcpy(&w, v + off + 4 * i, 4);
            n += snprintf(line + n, sizeof line - (size_t)n, " %08x", w);
        }
        bmvr_log("%s", line);
    }
    char fl[900]; int n = 0;
    for (unsigned off = 0; off < 0x150 && n < (int)sizeof fl - 40; off += 4) {
        const float f = vs_f(v, off);
        if (f == f && fabsf(f) > 0.01f && fabsf(f) < 1e5f) n += snprintf(fl + n, sizeof fl - (size_t)n, " %x=%.3g", off, (double)f);
    }
    bmvr_log("stereo:   floats:%s", fl);
}

/* Draw the pair. Returns 1 when the game's own RenderView must NOT be called afterwards (the eyes were drawn). */
static int st_try_stereo(void *self, void *view, int clear, int what)
{
    if (g_st_state != 1 || g_x_state != 1 || g_x_neyes != 2) return 0;
    if (!g_x_armed) { g_st_silent[0]++; return 0; }   /* the common case: not a delivery frame */
    if (!g_x_poses_on || !g_x_pose_pending) { st_note(0, "the helper has sent no pose yet"); return 0; }
    if (!(g_x_pose_pending_data.flags & BMVR_XFER_POSE_LOCAL)) { st_note(1, "the helper's poses are head-locked (VIEW space), not room-fixed (LOCAL)"); return 0; }
    if (g_spec[0].kind == SPEC_WINDOW) return 0;
    if (!xfer_gl_usable()) {
        if (!g_st_silent[1]++) bmvr_log("stereo: RenderView runs on thread %ld, which has no current GL context (the engine renders on another thread: mat_queue_mode). "
                                        "Mid-frame eye capture is impossible there; try `+mat_queue_mode 0` in the Steam launch options.", (long)syscall(SYS_gettid));
        return 0;
    }
    const unsigned char *vv = (const unsigned char *)view;
    if (vs_i(vv, VS_W) < 640 || vs_i(vv, VS_H) < 360) { g_st_pass_main++; g_st_silent[2]++; return 0; }   /* small views: reflections, monitors */
    if (!st_view_sane(vv)) { st_note(2, "the view does not look like a CViewSetup with the expected layout (size, fov, aspect, near/far, origin)"); return 0; }
    if (!xfer_peer_alive()) { g_st_silent[3]++; return 0; }     /* before any GPU wait: a dead helper never signals (see xfer_peer_alive) */

    if (g_st_recenter_req) { g_st_recenter_req = 0; g_st_rig.have_ref = 0; bmvr_log("stereo: recentered (right stick click)"); }
    /* the recenter trigger: touch <game folder>/bmvr_recenter */
    if ((g_st_frames % 30) == 0) {
        char dir[400], path[512];
        if (game_dir(dir, sizeof dir)) {
            snprintf(path, sizeof path, "%s/bmvr_recenter", dir);
            if (access(path, F_OK) == 0) { unlink(path); g_st_rig.have_ref = 0; bmvr_log("stereo: recentered on request"); }
        }
    }
    const struct bmvr_xfer_pose pose = g_x_pose_pending_data;
    const uint32_t pose_id = g_x_pose_pending;
    float go[3] = { vs_f(vv, VS_ORIGIN), vs_f(vv, VS_ORIGIN + 4), vs_f(vv, VS_ORIGIN + 8) };
    struct bmvr_view ev[2];
    if (!bmvr_stereo_eyes(&g_st_rig, &pose, go, vs_f(vv, VS_ANGLES + 4), ev)) { st_note(3, "the pose is unusable (bad quaternion or field of view)"); return 0; }

    if (!g_st_flag_on && g_st_flag_path[0]) {        /* crash-sticky: present until ST_FLAG_CLEAR_FRAMES good frames or a clean exit */
        int fd = open(g_st_flag_path, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) { close(fd); g_st_flag_on = 1; }
    }
    if (g_st_frames < 3)
        bmvr_log("stereo: frame %u pose %u: game view %dx%d fov %.1f origin (%.1f %.1f %.1f) yaw %.1f -> left eye origin (%.1f %.1f %.1f) ang (%.1f %.1f %.1f) "
                 "fov %.2f aspect %.3f frac l/r/b/t %.3f %.3f %.3f %.3f; right eye origin (%.1f %.1f %.1f)",
                 g_st_frames, pose_id, vs_i(vv, VS_W), vs_i(vv, VS_H), (double)vs_f(vv, VS_FOV), (double)go[0], (double)go[1], (double)go[2], (double)vs_f(vv, VS_ANGLES + 4),
                 (double)ev[0].origin[0], (double)ev[0].origin[1], (double)ev[0].origin[2], (double)ev[0].angles[0], (double)ev[0].angles[1], (double)ev[0].angles[2],
                 (double)ev[0].fov_deg, (double)ev[0].aspect, (double)ev[0].frac[0], (double)ev[0].frac[1], (double)ev[0].frac[2], (double)ev[0].frac[3],
                 (double)ev[1].origin[0], (double)ev[1].origin[1], (double)ev[1].origin[2]);

    struct glstate gs;
    struct eyeinfo ei[XFER_EYES];
    struct captarget cap[XFER_EYES];
    memset(ei, 0, sizeof ei);
    int ok = 1, synced = 0;
    g_x_t0 = now_ns();
    for (unsigned e = 0; e < 2 && ok; e++) {
        unsigned char buf[ST_VS_COPY];
        memcpy(buf, vv, sizeof buf);
        st_apply(buf, &ev[e]);
        g_cap[0].valid = g_cap[1].valid = 0;
        g_st_orig(self, buf, clear, what & ~ST_DRAW_HUD);
        if (!st_find_target(&cap[e])) { st_fail_eye(e); ok = 0; break; }
        if (g_st_frames < 3 && cap[e].w != (unsigned)vs_i(vv, VS_W))
            bmvr_log("stereo: eye %u: the target '%s' is %ux%u but the view is %dx%d; the whole target is blitted", e, cap[e].label, cap[e].w, cap[e].h, vs_i(vv, VS_W), vs_i(vv, VS_H));
        xfer_gl_save(&gs);
        ok = xfer_blit_eye(e, &cap[e], &synced, &ei[e]);
        xfer_gl_restore(&gs);
    }
    g_x_armed = 0;
    if (!ok) g_st_silent[4]++;
    if (ok) {
        unsigned semflag = xfer_sync_publish();
        if (xfer_send_frame(2, ei, semflag, pose_id)) {
            g_last_good[0] = cap[0]; g_last_good[0].valid = 1;
            g_last_good[1] = cap[1]; g_last_good[1].valid = 1;
            g_st_sent = 1;
            g_st_consec_fail = 0;
            g_st_frames++;
            g_st_ctx_tid = (long)syscall(SYS_gettid);
            if (g_st_frames == ST_FLAG_CLEAR_FRAMES && g_st_flag_on) { unlink(g_st_flag_path); g_st_flag_on = 0; }
            if (g_st_frames % 600 == 0) bmvr_log("stereo: %u stereo pairs delivered (%u dropped)", g_st_frames, g_st_fails);
        }
    }
    return 1;   /* the eyes were drawn; the game's own view is not drawn again */
}

#if defined(__i386__)
__attribute__((force_align_arg_pointer))
#endif
static void st_detour(void *self, void *view, int clear, int what)
{
    PFN_RenderView orig = g_st_orig;
    const long tid = (long)syscall(SYS_gettid);
    if (++g_st_calls == 1) bmvr_log("stereo: the hooked slot was called for the first time (tick %u, this=%p view=%p clear=%#x what=%#x); whether it really is RenderView is decided by the view layout check below", g_x_tick, self, view, (unsigned)clear, (unsigned)what);
    if (g_st_depth > 0) { g_st_nested++; orig(self, view, clear, what); return; }   /* nested view (water, monitor, cubemap), or another thread */
    g_st_depth = 1;
    g_st_tid = tid;
    g_st_top++;
    st_dump_view((const unsigned char *)view);
    if (!st_try_stereo(self, view, clear, what)) orig(self, view, clear, what);
    g_st_depth = 0;
}

static void __attribute__((destructor)) st_atexit(void)
{
    if (g_st_flag_on) unlink(g_st_flag_path);   /* a clean exit is not a crash */
}

struct st_list { unsigned n; };
/* Candidate view classes: every "<len>...ViewRender" name in the module, deduplicated. */
struct st_cands { char name[6][64]; int n; };
static void st_cand_cb(const char *mangled, uintptr_t addr, void *ud)
{
    (void)addr;
    struct st_cands *c = (struct st_cands *)ud;
    for (int i = 0; i < c->n; i++) if (!strcmp(c->name[i], mangled)) return;
    if (c->n < 6) snprintf(c->name[c->n++], sizeof c->name[0], "%.63s", mangled);
}
static void st_list_cb(const char *mangled, uintptr_t addr, void *ud)
{
    struct st_list *l = (struct st_list *)ud;
    if (l->n++ < 12) bmvr_log("stereo:   class name in the module: %s (string at %#lx)", mangled, (unsigned long)addr);
}

/* Detour the function's own first instructions: reaches RenderView however the engine calls it (vtable, direct call, inlined caller
 * that still calls the function). 32-bit only. g_st_orig becomes a trampoline holding the stolen instructions. */
static int st_install_inline(void)
{
    g_st_inline_tried = 1;
#if defined(__i386__)
    struct bmvr_map m;
    uintptr_t tramp = 0;
    if (!bmvr_maps_find(g_st_vt_old, &m) || m.perms[0] != 'r' || m.perms[2] != 'x') { bmvr_log("stereo: inline detour refused: the entry %#lx is not readable code", (unsigned long)g_st_vt_old); return 0; }
    if (!bmvr_inline_hook(g_st_vt_old, (uintptr_t)st_detour, m.perms, &tramp)) {
        bmvr_log("stereo: inline detour refused: the entry's first bytes (logged above) hold an instruction the decoder does not know to be position-independent "
                 "(a call/jump, or an unusual opcode). Send those bytes; the game is untouched and draws mono.");
        return 0;
    }
    g_st_orig = (PFN_RenderView)tramp;   /* after this, both the vtable slot and direct calls reach st_detour, and st_detour runs the original through the trampoline */
    bmvr_log("stereo: inline detour installed at %#lx (trampoline %#lx)", (unsigned long)g_st_vt_old, (unsigned long)tramp);
    return 1;
#else
    bmvr_log("stereo: the inline detour exists for the 32-bit game only; not available in this build");
    return 0;
#endif
}

/* Find the vtable, validate it, patch the RenderView slot. Every refusal says why and leaves the game untouched. */
static void st_try_install(void)
{
    if (!g_st_want || g_st_state != 0 || g_x_tick < g_st_next_try) return;
    g_st_state = -1;
    char dir[400];
    if (game_dir(dir, sizeof dir)) snprintf(g_st_flag_path, sizeof g_st_flag_path, "%s/bmvr_in_stereo.flag", dir);
    if (g_st_flag_path[0] && access(g_st_flag_path, F_OK) == 0) {
        unlink(g_st_flag_path);
        bmvr_log("stereo: bmvr_in_stereo.flag exists: the previous launch died while stereo was active. Stereo is OFF for this launch only "
                 "(the flag is removed; the next launch tries again).");
        return;
    }
    char mangled[80];
    snprintf(mangled, sizeof mangled, "%u%s", (unsigned)strlen(g_st_class), g_st_class);
    struct bmvr_map mod[BMVR_RTTI_MAX_RANGES];
    int nm = bmvr_maps_module(g_st_module, mod, BMVR_RTTI_MAX_RANGES);
    if (!nm) {
        /* The helper connects at the first Present, long before the engine has loaded client.so: look again every 60 frames. */
        static int said;
        if (!said) { said = 1; bmvr_log("stereo: no readable mapping of a module matching '%s' yet; trying again every 60 frames (a wrong stereo_module= never matches)", g_st_module); }
        g_st_state = 0;
        g_st_next_try = g_x_tick + 60;
        return;
    }
    struct bmvr_vt_hit h[8];
    int n = bmvr_rtti_find(g_st_module, mangled, h, 8), prim = -1, nprim = 0;
    for (int i = 0; i < n; i++) {
        bmvr_log("stereo: RTTI %s: type_info %#lx vtable %#lx (module offset %#lx) %s", mangled, (unsigned long)h[i].typeinfo, (unsigned long)h[i].vtable,
                 (unsigned long)(h[i].vtable - mod[0].lo), h[i].primary ? "primary" : "secondary");
        if (h[i].primary) { prim = i; nprim++; }
    }
    if (nprim != 1 && !g_st_class_explicit) {
        /* The default name is a guess (Black Mesa may call its class something else). Use another class only when it is unambiguous:
         * exactly one "*ViewRender" class that is not an interface (its slots are not all the same pure-virtual stub) and has one primary vtable. */
        struct st_cands cc; cc.n = 0;
        bmvr_rtti_list(g_st_module, "ViewRender", st_cand_cb, &cc);
        int pick = -1, ok = 0;
        struct bmvr_vt_hit ch[8];
        for (int ci = 0; ci < cc.n; ci++) {
            int m = bmvr_rtti_find(g_st_module, cc.name[ci], ch, 8), np = 0, pi = -1;
            for (int i = 0; i < m; i++) if (ch[i].primary) { np++; pi = i; }
            if (np != 1) continue;
            const uintptr_t *v = (const uintptr_t *)ch[pi].vtable;
            int usable = bmvr_addr_executable(v[g_st_slot]) && bmvr_addr_executable(v[g_st_slot - 1 >= 0 ? g_st_slot - 1 : 0]);
            if (usable && g_st_slot > 0 && v[g_st_slot] == v[g_st_slot - 1]) usable = 0;   /* identical neighbours: an interface of pure virtuals */
            const char *id = cc.name[ci];
            while (*id >= '0' && *id <= '9') id++;
            if (id[0] == 'I' && id[1] >= 'A' && id[1] <= 'Z') usable = 0;                    /* Source naming: IViewRender is the interface, not the implementation */
            bmvr_log("stereo: candidate class %s: %d vtable(s), %s", cc.name[ci], m, usable ? "usable" : "not usable (interface or short vtable)");
            if (usable) { ok++; pick = ci; }
        }
        if (ok == 1) {
            bmvr_log("stereo: '%s' not found; using the only usable *ViewRender class %s (set stereo_class= to override)", g_st_class, cc.name[pick]);
            snprintf(mangled, sizeof mangled, "%s", cc.name[pick]);
            n = bmvr_rtti_find(g_st_module, mangled, h, 8); prim = -1; nprim = 0;
            for (int i = 0; i < n; i++) {
                bmvr_log("stereo: RTTI %s: type_info %#lx vtable %#lx (module offset %#lx) %s", mangled, (unsigned long)h[i].typeinfo, (unsigned long)h[i].vtable,
                         (unsigned long)(h[i].vtable - mod[0].lo), h[i].primary ? "primary" : "secondary");
                if (h[i].primary) { prim = i; nprim++; }
            }
        } else if (cc.n) {
            bmvr_log("stereo: %d usable *ViewRender classes; not guessing (set stereo_class= to one of the names above, without the length prefix)", ok);
        }
    }
    if (nprim != 1) {
        struct st_list l = { 0 };
        bmvr_log("stereo: class '%s' has %d primary vtables in '%s' (need exactly 1); stereo is off. Classes whose name ends in ViewRender:", g_st_class, nprim, g_st_module);
        bmvr_rtti_list(g_st_module, "ViewRender", st_list_cb, &l);
        if (!l.n) bmvr_log("stereo:   (none: the module may not carry RTTI, or stereo_module= names the wrong file)");
        return;
    }
    uintptr_t *vt = (uintptr_t *)h[prim].vtable;
    for (int i = 0; i <= g_st_slot + 1; i++)   /* never read past the slot we were asked to use, plus one neighbour for the log */
        bmvr_log("stereo:   slot %2d -> %#lx (module offset %#lx)%s", i, (unsigned long)vt[i], (unsigned long)(vt[i] - mod[0].lo), i == g_st_slot ? "   <- RenderView?" : "");
    for (int i = 0; i <= g_st_slot; i++)
        if (!bmvr_addr_executable(vt[i])) { bmvr_log("stereo: vtable slot %d is not executable code: the vtable is shorter than stereo_slot=%d; stereo is off", i, g_st_slot); return; }
    uintptr_t old = 0;
    bmvr_stereo_rig_init(&g_st_rig, g_st_scale);
    g_st_orig = (PFN_RenderView)vt[g_st_slot];
    g_st_vt_old = vt[g_st_slot];
    {   /* the real entry's first bytes: needed to write an inline detour for this build if the decoder refuses them */
        const uint8_t *c = (const uint8_t *)vt[g_st_slot];
        bmvr_log("stereo: RenderView entry %#lx bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", (unsigned long)vt[g_st_slot],
                 c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
        struct st_list l = { 0 };
        bmvr_rtti_list(g_st_module, "ViewRender", st_list_cb, &l);   /* a derived class that overrides RenderView would never use this vtable */
    }
    g_st_state = 1;
    g_st_hook_tick = g_x_tick;
    if (g_st_hook_mode == 2) {
        if (!st_install_inline()) { g_st_state = -1; g_st_orig = NULL; return; }
        bmvr_log("stereo: inline detour on RenderView installed (stereo_hook=inline); the vtable slot is left alone");
        return;
    }
    if (!bmvr_vtable_patch(&vt[g_st_slot], (uintptr_t)st_detour, &old)) { bmvr_log("stereo: could not patch the vtable slot (mprotect refused); stereo is off"); g_st_orig = NULL; g_st_state = -1; return; }
    g_st_vt_slot = &vt[g_st_slot];
    bmvr_log("stereo: hooked %s vtable slot %d (was %#lx). Each delivery frame now draws two eyes from the helper's pose. UNVERIFIED on this game build: "
             "check the 'frame 0' line below and the picture in the headset.", mangled, g_st_slot, (unsigned long)old);
}

/* Once per ~10 s at 60 fps while stereo is requested: where things stand, and a loud warning when the slot is never called. */
static void st_status_tick(void)
{
    /* auto mode: the slot was patched but nothing called it for 300 frames (10 s at 30 fps): the engine does not go through the vtable. */
    if (g_st_want && g_st_state == 1 && g_st_hook_mode == 0 && !g_st_inline_tried && !g_st_calls && g_x_tick - g_st_hook_tick >= 300) {
        bmvr_log("stereo: the patched slot was not called in %u frames; trying an inline detour on the function itself (stereo_hook=vtable turns this off)", g_x_tick - g_st_hook_tick);
        if (st_install_inline()) g_st_hook_tick = g_x_tick;
    }
    if (!g_st_want || g_st_state != 1 || g_x_tick == g_st_hook_tick || (g_x_tick - g_st_hook_tick) % 600) return;
    bmvr_log("stereo: status tick %u: RenderView calls %u (nested %u, top-level %u); stereo pairs %u (%u dropped); let through because: no pose %u, head-locked pose %u, "
             "view layout refused %u, bad pose %u", g_x_tick, g_st_calls, g_st_nested, g_st_top, g_st_frames, g_st_fails, g_st_why_cnt[0], g_st_why_cnt[1], g_st_why_cnt[2], g_st_why_cnt[3]);
    bmvr_log("stereo:   silent exits: not armed %u, no GL context %u, small view %u, helper gone %u, blit failed %u; last pair drawn on thread %ld, RenderView thread now %ld",
             g_st_silent[0], g_st_silent[1], g_st_silent[2], g_st_silent[3], g_st_silent[4], g_st_ctx_tid, g_st_tid);
    if (!g_st_calls && !g_st_never_warned && g_x_tick - g_st_hook_tick >= 1800) {
        g_st_never_warned = 1;
        bmvr_log("stereo: WARNING: the hooked vtable slot has not been called once in %u frames. If a map is loaded, the engine does not reach RenderView through this "
                 "slot: wrong class or stereo_slot= (compare the slot dump above), or the engine calls RenderView directly (inlined). The game is drawing mono.", g_x_tick - g_st_hook_tick);
    }
}

/* Is the helper still connected? The kernel closes the socket when the helper process dies, so a non-blocking
 * peek tells us BEFORE the GPU is asked to wait on a semaphore the dead helper will never signal (a GL wait has
 * no timeout; the game would hang). Called once per snapshot, not per Present. */
static int xfer_peer_alive(void)
{
    char c;
    ssize_t n = recv(g_x_sock, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        bmvr_log("xfer: the helper's socket is closed (helper exited); not waiting on its GPU semaphore; back to searching");
        xfer_teardown();
        g_x_state = 0;
        return 0;
    }
    return 1;
}

/* v5 hook-port interface: the newest pose the helper sent (what the NEXT frame should be rendered with; its id is what gets stamped on
 * that frame). Returns 1 and fills *out, or 0 when the helper offers no poses (or has not sent one yet). Poses are OpenXR-convention
 * (right-handed, +Y up, -Z forward); the conversion to Source space belongs to the caller. Nothing in the shim calls this yet. */
BMVR_EXPORT int bmvr_get_render_pose(struct bmvr_xfer_pose *out)
{
    if (!g_x_poses_on || !g_x_pose_pending) return 0;
    *out = g_x_pose_pending_data;
    return 1;
}

/* ------------------------------------------------------------ slice 3b: controller input into CreateMove
 * IClientMode::CreateMove(float flInputSampleTime, CUserCmd *cmd) -> bool. i386 System V: a plain cdecl call with `this` as the first stack
 * argument, so an ordinary C function of the same shape can stand in for it.
 *
 * Safety, because the slot number is a guess on this game build:
 *   1. the detour forwards its three arguments unchanged and returns what the original returned, so a wrong slot behaves like the real
 *      function for every callee that takes (this, float, pointer) and returns an integer;
 *   2. for IC_LEARN consecutive calls it only LOOKS at the second pointer: it must be readable memory laid out like a CUserCmd (vptr in a
 *      mapped region, finite view angles, plausible numbers, a command number that never goes backwards). Only then does it write;
 *   3. one bad call after that (the cmd stops looking like a cmd) switches writing off for good, and says so;
 *   4. a flag file marks "input was active": a launch that died with it set runs without input once (like the stereo flag). */
struct bmvr_usercmd {            /* mirrors src/sdk/usercmd.h: virtual destructor first, then the fields in the same order */
    void *vptr;
    int command_number, tick_count;
    float viewangles[3];
    float forwardmove, sidemove, upmove;
    int buttons;
    unsigned char impulse;
    int weaponselect, weaponsubtype, random_seed;
    short mousedx, mousedy;
    unsigned char hasbeenpredicted;
};
#define IC_LEARN 20
#define IC_FLAG_CLEAR_CALLS 1800
typedef int (*PFN_CreateMove)(void *, float, void *);
static int g_ic_state;                 /* 0 = not tried, 1 = hooked, -1 = refused / failed */
#define IC_MAXH 8
/* One entry per patched vtable slot: the base class and every derived "ClientMode*" class that shares its layout. A derived class keeps its OWN copy
 * of the vtable, so patching only ClientModeShared's vtable is never seen when the game instantiates ClientModeBMSNormal or similar. */
struct ic_hook { uintptr_t *slot; PFN_CreateMove orig; unsigned calls, sane_run, insane, bad_run; int last_cmd; char cls[96]; };
static struct ic_hook g_ic_h[IC_MAXH];
static int g_ic_nh;
static int g_ic_junk_said;
static int g_ic_win = -1;              /* the hook that proved to be CreateMove */
static unsigned g_ic_next_try, g_ic_hook_tick;
static unsigned g_ic_calls, g_ic_applied, g_ic_nocmd;
static int g_ic_armed;                 /* the learning phase passed: values are written */
static int g_ic_dead;                  /* armed, then a cmd failed the check: never write again */
static char g_ic_flag_path[512];
static int g_ic_flag_on;
static struct bmvr_input_map g_ic_map;
static int g_ic_map_ready;
static int g_ic_snap_latched;

static int ic_cmd_sane(const struct bmvr_usercmd *c)
{
    struct bmvr_map m;
    if (!c || ((uintptr_t)c & 3u)) return 0;
    if (!bmvr_maps_find((uintptr_t)c, &m) || m.perms[0] != 'r' || m.perms[1] != 'w') return 0;
    if ((uintptr_t)c + sizeof *c > m.hi) return 0;
    if (!c->vptr || !bmvr_maps_find((uintptr_t)c->vptr, &m) || m.perms[0] != 'r') return 0;
    if (c->command_number < 0 || c->command_number > 0x3fffffff || c->tick_count < 0 || c->tick_count > 0x3fffffff) return 0;
    for (int i = 0; i < 3; i++) if (!(fabsf(c->viewangles[i]) < 100000.0f)) return 0;      /* also rejects NaN */
    if (!(fabsf(c->viewangles[0]) <= 180.0f)) return 0;
    if (!(fabsf(c->forwardmove) <= 5000.0f) || !(fabsf(c->sidemove) <= 5000.0f) || !(fabsf(c->upmove) <= 5000.0f)) return 0;
    if ((unsigned)c->buttons > 0x3ffffffu) return 0;
    return 1;
}

/* Weapon switching goes through SDL like the menu does (defined with the menu code below): prev / next = mouse wheel, wheel pick = number keys 1-6 */
#define IC_KEY_SLOT0 6
static void ic_menu_key_pulse(int key);
static void ic_sdl_wheel(int dir);
static float g_ic_wheel_x, g_ic_wheel_y;   /* right stick while the weapon wheel is open */

/* Write the controllers into one cmd. Everything the keyboard and mouse already put there stays: ours is added on top. */
static void ic_apply(float dt, struct bmvr_usercmd *cmd)
{
    struct bmvr_xfer_input in;
    if (!bmvr_get_input(&in)) { g_ic_nocmd++; return; }
    {   struct bmvr_xfer_input again;   /* the render thread may be writing it: take it only if it did not move while we copied */
        if (!bmvr_get_input(&again) || again.seq != in.seq) in = again;
    }
    if (!g_ic_map_ready) {
        bmvr_input_map_init(&g_ic_map);
        g_ic_map.cfg.walk_dead = g_ic_deadzone;
        g_ic_map.cfg.trigger_on = g_ic_trigger;
        g_ic_map.cfg.trigger_off = g_ic_trigger - 0.2f;
        g_ic_map.cfg.act_a = g_ic_act[0]; g_ic_map.cfg.act_b = g_ic_act[1]; g_ic_map.cfg.act_bumper = g_ic_act[2];
        g_ic_map.cfg.act_x = g_ic_act[3]; g_ic_map.cfg.act_y = g_ic_act[4];
        g_ic_map_ready = 1;
    }
    {   /* hands: weapon_hand=left mirrors everything; move_hand equal to the weapon hand puts walking and firing on one hand */
        const int weapon = g_ic_weapon_left ? 0 : 1;
        const int move = g_ic_move_hand >= 0 ? g_ic_move_hand : 1 - weapon;
        bmvr_im_remap_hands(&in, g_ic_weapon_left, move == weapon);
    }
    /* Real time since the previous call, from the wall clock: turning is degrees per SECOND times that time, so the total turn is the same at any frame
     * rate and under load (the engine's flInputSampleTime is not always the real frame time, and a fixed fallback made the speed follow the frame rate).
     * Not clamped upwards below a quarter second, so a hitch still turns the view by the time it took; a longer gap (pause, loading) counts as one frame. */
    float dt_ms;
    {   static uint64_t last_ns;
        const uint64_t n = now_ns();
        dt_ms = last_ns && n >= last_ns ? (float)((double)(n - last_ns) / 1e6) : 11.0f;
        last_ns = n;
        if (dt_ms > 250.0f) dt_ms = 11.0f;
        (void)dt;
    }
    struct bmvr_cmd c;
    bmvr_input_map_step(&g_ic_map, &in, dt_ms, &c);

    /* movement: the stick is relative to where the HEAD looks; the cmd is relative to the game's yaw (cmd->viewangles[1]).
     * delta = head yaw change since the stereo recenter (0 when there is no head reference). */
    float delta = 0.0f;
    if (g_st_rig.have_ref && (g_x_pose_pending_data.flags & BMVR_XFER_POSE_LOCAL)) {
        float pos[3], fwd[3];
        if (bmvr_stereo_head(&g_x_pose_pending_data, g_st_rig.scale, pos, fwd)) {
            delta = atan2f(fwd[1], fwd[0]) * 180.0f / BMVR_PI_F - g_st_rig.ref_yaw;
            while (delta > 180.0f) delta -= 360.0f;
            while (delta < -180.0f) delta += 360.0f;
        }
    }
    float fm, sm;
    bmvr_im_move_relative(c.forward, c.side, delta, 450.0f, &fm, &sm);
    cmd->forwardmove += fm;
    cmd->sidemove += sm;

    /* turning: Source yaw is positive to the LEFT, so a right-stick push SUBTRACTS */
    if (g_ic_snap > 0.0f) {
        if (fabsf(c.turn) < 0.3f) g_ic_snap_latched = 0;
        else if (fabsf(c.turn) > 0.7f && !g_ic_snap_latched) { g_ic_snap_latched = 1; cmd->viewangles[1] -= (c.turn > 0.0f ? 1.0f : -1.0f) * g_ic_snap; }
    } else {
        const float dir = g_ic_turn_analog ? c.turn : (c.turn > 0.0f ? 1.0f : c.turn < 0.0f ? -1.0f : 0.0f);   /* constant speed: past the deadzone it is all or nothing */
        cmd->viewangles[1] -= dir * g_ic_turn_speed * (dt_ms / 1000.0f);
    }
    while (cmd->viewangles[1] > 180.0f) cmd->viewangles[1] -= 360.0f;
    while (cmd->viewangles[1] < -180.0f) cmd->viewangles[1] += 360.0f;

    cmd->buttons |= (int)c.buttons;
    if (c.events & BMVR_EV_FLASHLIGHT) cmd->impulse = 100;
    if (c.events & BMVR_EV_RECENTER) g_st_recenter_req = 1;
    /* BMVR_EV_PAUSE: handled in ic_menu_tick (Present rate, so it also works while the game is paused or in a menu and CreateMove does not run) */
    if (c.events & BMVR_EV_WEAPON_PREV) ic_sdl_wheel(1);      /* wheel up = invprev */
    if (c.events & BMVR_EV_WEAPON_NEXT) ic_sdl_wheel(-1);     /* wheel down = invnext */
    if (c.wheel_open || (c.events & BMVR_EV_WHEEL_OPEN)) { g_ic_wheel_x = in.stick[1][0]; g_ic_wheel_y = in.stick[1][1]; }
    if (c.events & BMVR_EV_WHEEL_OPEN) { static int said; if (!said++) bmvr_log("input: weapon wheel: hold the right stick click, push the stick toward a slot, release to pick (6 sectors, clockwise from the top = slots 1-6; no wheel graphic yet)"); }
    if (c.events & BMVR_EV_WHEEL_SELECT) {
        const float m = sqrtf(g_ic_wheel_x * g_ic_wheel_x + g_ic_wheel_y * g_ic_wheel_y);
        if (m > 0.5f) {
            float a = atan2f(g_ic_wheel_x, g_ic_wheel_y) * 180.0f / BMVR_PI_F;   /* 0 = up, clockwise */
            if (a < 0.0f) a += 360.0f;
            int slot = (int)((a + 30.0f) / 60.0f) % 6;
            ic_menu_key_pulse(IC_KEY_SLOT0 + slot);
            bmvr_log("input: weapon wheel -> slot %d", slot + 1);
        }
        g_ic_wheel_x = g_ic_wheel_y = 0.0f;
    }
    g_ic_applied++;
}

static int ic_detour_n(int i, void *self, float t, void *cmdp)
{
    struct ic_hook *h = &g_ic_h[i];
    struct bmvr_usercmd *cmd = (struct bmvr_usercmd *)cmdp;
    h->calls++;
    g_ic_calls++;
    const int sane = ic_cmd_sane(cmd);
    const int mine = g_ic_armed && g_ic_win == i;
    if (mine && !g_ic_dead && sane && cmd->command_number != 0) {
        if (cmd->command_number >= h->last_cmd) h->last_cmd = cmd->command_number;
        ic_apply(t, cmd);
    }
    const int r = h->orig(self, t, cmdp);
    if (!g_ic_armed) {
        /* learning: judge the cmd AFTER the original ran, and never write */
        if (sane && cmd->command_number >= h->last_cmd && cmd->command_number != 0) {
            h->last_cmd = cmd->command_number;
            if (++h->sane_run == IC_LEARN) {
                g_ic_win = i;
                g_ic_armed = 1;
                bmvr_log("input: the hooked slot %d of %s looks like CreateMove (%d consecutive calls with a plausible, increasing CUserCmd). Controller input is now written "
                         "into the game: left stick walks, right stick turns, right trigger fires.", g_ic_slot, h->cls, IC_LEARN);
                for (int j = 0; j < g_ic_nh; j++)   /* the others were only watching: put their slots back */
                    if (j != i && g_ic_h[j].slot) { uintptr_t old; bmvr_vtable_patch(g_ic_h[j].slot, (uintptr_t)g_ic_h[j].orig, &old); g_ic_h[j].slot = NULL; }
                if (g_ic_flag_path[0]) { int fd = open(g_ic_flag_path, O_WRONLY | O_CREAT, 0644); if (fd >= 0) { close(fd); g_ic_flag_on = 1; } }
            }
        } else if (cmd && cmd->command_number != 0 && h->sane_run) { h->sane_run = 0; h->insane++; }
        else if (!sane && !h->insane++ && !g_ic_junk_said++) bmvr_log("input: the second argument of the hooked slot's calls does not look like a CUserCmd yet (watching; nothing is written)");
    } else if (mine && !g_ic_dead) {
        if (sane) h->bad_run = 0;
        else if (++h->bad_run >= 3) {
            g_ic_dead = 1;
            bmvr_log("input: a call of the hooked slot no longer looks like CreateMove; input is OFF for the rest of this launch (wrong input_slot=?)");
        }
    }
    return r;
}
#define IC_TR(n) static int ic_tr##n(void *s, float t, void *c) { return ic_detour_n(n, s, t, c); }
IC_TR(0) IC_TR(1) IC_TR(2) IC_TR(3) IC_TR(4) IC_TR(5) IC_TR(6) IC_TR(7)
static const PFN_CreateMove g_ic_tr[IC_MAXH] = { ic_tr0, ic_tr1, ic_tr2, ic_tr3, ic_tr4, ic_tr5, ic_tr6, ic_tr7 };

static void __attribute__((destructor)) ic_atexit(void)
{
    if (g_ic_flag_on) unlink(g_ic_flag_path);   /* a clean exit is not a crash */
}

static void ic_try_install(void)
{
    if (!g_ic_want || g_ic_state != 0 || g_x_tick < g_ic_next_try) return;
    g_ic_state = -1;
    char dir[400];
    if (game_dir(dir, sizeof dir)) snprintf(g_ic_flag_path, sizeof g_ic_flag_path, "%s/bmvr_in_input.flag", dir);
    if (g_ic_flag_path[0] && access(g_ic_flag_path, F_OK) == 0) {
        unlink(g_ic_flag_path);
        bmvr_log("input: bmvr_in_input.flag exists: the previous launch died while controller input was active. Input is OFF for this launch only "
                 "(the flag is removed; the next launch tries again). If it repeats, set input_slot= or input=0.");
        return;
    }
    struct bmvr_map mod[BMVR_RTTI_MAX_RANGES];
    if (!bmvr_maps_module(g_ic_module, mod, BMVR_RTTI_MAX_RANGES)) {
        static int said;
        if (!said) { said = 1; bmvr_log("input: no readable mapping of a module matching '%s' yet; trying again every 60 frames", g_ic_module); }
        g_ic_state = 0;
        g_ic_next_try = g_x_tick + 60;
        return;
    }
    char mangled[80];
    snprintf(mangled, sizeof mangled, "%u%s", (unsigned)strlen(g_ic_class), g_ic_class);
    struct bmvr_vt_hit h[8];
    int n = bmvr_rtti_find(g_ic_module, mangled, h, 8), prim = -1, nprim = 0;
    for (int i = 0; i < n; i++) {
        bmvr_log("input: RTTI %s: type_info %#lx vtable %#lx (module offset %#lx) %s", mangled, (unsigned long)h[i].typeinfo, (unsigned long)h[i].vtable,
                 (unsigned long)(h[i].vtable - mod[0].lo), h[i].primary ? "primary" : "secondary");
        if (h[i].primary) { prim = i; nprim++; }
    }
    if (nprim != 1) {
        bmvr_log("input: class '%s' has %d primary vtables in '%s' (need exactly 1); input is off. Classes whose name contains ClientMode:", g_ic_class, nprim, g_ic_module);
        struct st_list l = { 0 };
        bmvr_rtti_list(g_ic_module, "ClientMode", st_list_cb, &l);
        bmvr_rtti_list(g_ic_module, "ClientModeShared", st_list_cb, &l);
        if (!l.n) bmvr_log("input:   (none: the module may not carry RTTI, or input_module= names the wrong file)");
        else bmvr_log("input:   pick the one the game really uses with input_class= (without the length prefix) and send me this log");
        return;
    }
    uintptr_t *vt = (uintptr_t *)h[prim].vtable;
    for (int i = 0; i <= g_ic_slot + 2; i++)
        bmvr_log("input:   slot %2d -> %#lx (module offset %#lx)%s", i, (unsigned long)vt[i], (unsigned long)(vt[i] - mod[0].lo), i == g_ic_slot ? "   <- CreateMove?" : "");
    for (int i = 0; i <= g_ic_slot; i++)
        if (!bmvr_addr_executable(vt[i])) { bmvr_log("input: vtable slot %d is not executable code: the vtable is shorter than input_slot=%d; input is off", i, g_ic_slot); return; }
    g_ic_state = 1;
    g_ic_hook_tick = g_x_tick;
    uintptr_t old = 0;
    {   struct ic_hook *h = &g_ic_h[0];
        h->orig = (PFN_CreateMove)vt[g_ic_slot];
        snprintf(h->cls, sizeof h->cls, "%s", mangled);
        if (!bmvr_vtable_patch(&vt[g_ic_slot], (uintptr_t)g_ic_tr[0], &old)) { bmvr_log("input: could not patch the vtable slot (mprotect refused); input is off"); g_ic_state = -1; return; }
        h->slot = &vt[g_ic_slot];
        g_ic_nh = 1;
    }
    bmvr_log("input: hooked %s vtable slot %d (was %#lx). WATCHING ONLY until %d consecutive calls look like CreateMove; then the controllers are written into the game. "
             "UNVERIFIED on this build.", mangled, g_ic_slot, (unsigned long)old, IC_LEARN);

    /* Derived classes. The engine instantiates a subclass (ClientModeHL2MPNormal and the like), whose vtable is a separate copy: the base class's patched
     * slot is then never called. Also watch the same slot of every other "ClientMode" class whose vtable has the base layout (most slots identical). */
    if (g_ic_probe) {
        struct bmvr_vt_hit dh[IC_MAXH];
        const int nd = bmvr_rtti_derived(g_ic_module, h[prim].typeinfo, dh, IC_MAXH);
        if (!nd) bmvr_log("input: no subclass of %s found in '%s' (the game instantiates the base class itself)", mangled, g_ic_module);
        for (int ci = 0; ci < nd && g_ic_nh < IC_MAXH; ci++) {
            const char *dname = (const char *)((const uintptr_t *)dh[ci].typeinfo)[1];
            struct { char name[64]; } cc1; snprintf(cc1.name, sizeof cc1.name, "%.63s", dname);
            uintptr_t *dv = (uintptr_t *)dh[ci].vtable;
            int ok = 1, same = 0;
            for (int i = 0; i <= g_ic_slot; i++) { if (!bmvr_addr_executable(dv[i])) ok = 0; else if (dv[i] == (i == g_ic_slot ? (uintptr_t)g_ic_h[0].orig : vt[i])) same++; }   /* the base slot now holds our detour */
            const int total = g_ic_slot + 1;
            bmvr_log("input: candidate class %s: vtable %#lx, %d of %d slots identical to %s, slot %d -> %#lx%s", cc1.name, (unsigned long)dv, same, total, mangled, g_ic_slot,
                     (unsigned long)dv[g_ic_slot], dv[g_ic_slot] == (uintptr_t)g_ic_h[0].orig ? " (same function as the base)" : " (its own override)");
            if (!ok || same * 5 < total * 2) { bmvr_log("input:   not watched (%s)", !ok ? "vtable too short" : "layout differs from the base: not a subclass"); continue; }
            struct ic_hook *ih = &g_ic_h[g_ic_nh];
            ih->orig = (PFN_CreateMove)dv[g_ic_slot];
            snprintf(ih->cls, sizeof ih->cls, "%s", cc1.name);
            if (!bmvr_vtable_patch(&dv[g_ic_slot], (uintptr_t)g_ic_tr[g_ic_nh], &old)) { bmvr_log("input:   could not patch (mprotect refused); not watched"); continue; }
            ih->slot = &dv[g_ic_slot];
            g_ic_nh++;
            bmvr_log("input:   watched as well (hook %d)", g_ic_nh - 1);
        }
    }
}

/* Game menu control. Everything goes through SDL's event queue (the engine reads keyboard and mouse from there). Runs once per Present, not from CreateMove,
 * which does not run while a menu is up or the game is paused.
 *   Menu / view button : Escape (open / close the game menu, always)
 * While a menu owns the screen (CreateMove has stopped being called for MENU_STALL Presents, which also covers the main menu and loading screens):
 *   left stick / d-pad : arrow keys (with key repeat)
 *   right stick        : moves the mouse cursor
 *   either trigger     : left mouse button (hold to drag sliders)
 *   A                  : Enter
 *   B                  : Escape (back) */
typedef int (*PFN_SDL_PushEvent)(void *);
typedef void *(*PFN_SDL_GetKeyboardFocus)(void);
typedef void *(*PFN_SDL_GetMouseFocus)(void);
typedef unsigned (*PFN_SDL_GetWindowID)(void *);
typedef void (*PFN_SDL_GetWindowSize)(void *, int *, int *);
typedef void (*PFN_SDL_WarpMouseInWindow)(void *, int, int);
static PFN_SDL_PushEvent g_sdl_push;
static PFN_SDL_GetKeyboardFocus g_sdl_focus;
static PFN_SDL_GetMouseFocus g_sdl_mfocus;
static PFN_SDL_GetWindowID g_sdl_winid;
static PFN_SDL_GetWindowSize g_sdl_winsize;
static PFN_SDL_WarpMouseInWindow g_sdl_warp;
static int g_sdl_tried;

#define MENU_STALL 6                 /* Presents without a CreateMove call before the controllers drive the menu */
#define MENU_REPEAT_DELAY 30
#define MENU_REPEAT_EVERY 8
#define MENU_KEYS 12
enum { MK_ESC, MK_ENTER, MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT };
static const struct { int scan; int sym; } g_menu_key_tab[MENU_KEYS] = {
    { 41, 27 }, { 40, 13 }, { 82, 0x40000052 }, { 81, 0x40000051 }, { 80, 0x40000050 }, { 79, 0x4000004F },
    { 30, '1' }, { 31, '2' }, { 32, '3' }, { 33, '4' }, { 34, '5' }, { 35, '6' },
};
static unsigned g_menu_release_tick[MENU_KEYS];   /* 0 = key is up */
static unsigned g_menu_hold_since[MENU_KEYS], g_menu_next_rep[MENU_KEYS];
static unsigned g_menu_prev_down, g_menu_presses, g_menu_prev_calls, g_menu_stalled;
static int g_menu_have_prev, g_menu_active, g_menu_mouse_down, g_menu_logged_on;
static float g_menu_mx = -1.0f, g_menu_my;
static uint64_t g_menu_last_ns;

static void ic_sdl_init(void)
{
    if (g_sdl_tried) return;
    g_sdl_tried = 1;
    void *h = NULL;
    g_sdl_push = (PFN_SDL_PushEvent)dlsym(RTLD_DEFAULT, "SDL_PushEvent");
    if (!g_sdl_push) { h = find_loaded_lib("libSDL2-2.0.so.0", "libSDL2"); if (h) g_sdl_push = (PFN_SDL_PushEvent)dlsym(h, "SDL_PushEvent"); }
    if (g_sdl_push) {
        void *d = h ? h : RTLD_DEFAULT;
        g_sdl_focus = (PFN_SDL_GetKeyboardFocus)dlsym(d, "SDL_GetKeyboardFocus");
        g_sdl_mfocus = (PFN_SDL_GetMouseFocus)dlsym(d, "SDL_GetMouseFocus");
        g_sdl_winid = (PFN_SDL_GetWindowID)dlsym(d, "SDL_GetWindowID");
        g_sdl_winsize = (PFN_SDL_GetWindowSize)dlsym(d, "SDL_GetWindowSize");
        g_sdl_warp = (PFN_SDL_WarpMouseInWindow)dlsym(d, "SDL_WarpMouseInWindow");
    } else bmvr_log("input: the controllers cannot drive the game menu: SDL_PushEvent was not found");
}

static void *ic_sdl_window(void)
{
    void *w = g_sdl_focus ? g_sdl_focus() : NULL;
    if (!w && g_sdl_mfocus) w = g_sdl_mfocus();
    return w;
}

static void ic_sdl_key(int key, int down)
{
    ic_sdl_init();
    if (!g_sdl_push || key < 0 || key >= MENU_KEYS) return;
    unsigned char ev[56];                       /* SDL_Event: 56 bytes; SDL_KeyboardEvent uses the first 32 */
    memset(ev, 0, sizeof ev);
    uint32_t type = down ? 0x300u : 0x301u;     /* SDL_KEYDOWN / SDL_KEYUP */
    uint32_t win = 0;
    void *w = ic_sdl_window();
    if (w && g_sdl_winid) win = g_sdl_winid(w);
    int32_t scancode = g_menu_key_tab[key].scan, sym = g_menu_key_tab[key].sym;
    memcpy(ev + 0, &type, 4);
    memcpy(ev + 8, &win, 4);
    ev[12] = down ? 1 : 0;                      /* state */
    memcpy(ev + 16, &scancode, 4);
    memcpy(ev + 20, &sym, 4);
    g_sdl_push(ev);
}

/* a key press that stays down for a couple of frames, like a real one */
static void ic_menu_key_pulse(int key)
{
    if (g_menu_release_tick[key]) return;
    ic_sdl_key(key, 1);
    g_menu_release_tick[key] = g_x_tick + 2;
}

static void ic_sdl_mouse(int down_edge, int up_edge)
{
    ic_sdl_init();
    void *w = ic_sdl_window();
    if (!g_sdl_push || !w) return;
    int ww = 0, wh = 0;
    if (g_sdl_winsize) g_sdl_winsize(w, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;
    if (g_menu_mx < 0.0f) { g_menu_mx = ww * 0.5f; g_menu_my = wh * 0.5f; }
    if (g_menu_mx > ww - 1) g_menu_mx = (float)(ww - 1);
    if (g_menu_my > wh - 1) g_menu_my = (float)(wh - 1);
    if (g_menu_mx < 0.0f) g_menu_mx = 0.0f;
    if (g_menu_my < 0.0f) g_menu_my = 0.0f;
    const int x = (int)g_menu_mx, y = (int)g_menu_my;
    const uint32_t win = g_sdl_winid ? g_sdl_winid(w) : 0;
    if (g_sdl_warp) g_sdl_warp(w, x, y);        /* SDL itself queues the SDL_MOUSEMOTION for a warp */
    else {
        unsigned char ev[56]; memset(ev, 0, sizeof ev);
        uint32_t type = 0x400u, st = g_menu_mouse_down ? 1u : 0u;   /* SDL_MOUSEMOTION */
        int32_t xx = x, yy = y;
        memcpy(ev + 0, &type, 4); memcpy(ev + 8, &win, 4); memcpy(ev + 16, &st, 4); memcpy(ev + 20, &xx, 4); memcpy(ev + 24, &yy, 4);
        g_sdl_push(ev);
    }
    if (down_edge || up_edge) {
        unsigned char ev[56]; memset(ev, 0, sizeof ev);
        uint32_t type = down_edge ? 0x401u : 0x402u;                /* SDL_MOUSEBUTTONDOWN / UP */
        int32_t xx = x, yy = y;
        memcpy(ev + 0, &type, 4); memcpy(ev + 8, &win, 4);
        ev[16] = 1;                                                 /* SDL_BUTTON_LEFT */
        ev[17] = down_edge ? 1 : 0;                                 /* state */
        ev[18] = 1;                                                 /* clicks */
        memcpy(ev + 20, &xx, 4); memcpy(ev + 24, &yy, 4);
        g_sdl_push(ev);
    }
}

static void ic_sdl_wheel(int dir)
{
    ic_sdl_init();
    if (!g_sdl_push) return;
    unsigned char ev[56]; memset(ev, 0, sizeof ev);
    uint32_t type = 0x403u, win = 0;                       /* SDL_MOUSEWHEEL */
    void *w = ic_sdl_window();
    if (w && g_sdl_winid) win = g_sdl_winid(w);
    int32_t y = dir;
    memcpy(ev + 0, &type, 4); memcpy(ev + 8, &win, 4); memcpy(ev + 20, &y, 4);
    g_sdl_push(ev);
}

/* one controller direction as a key with initial delay and repeat */
static void ic_menu_dir(int key, int on)
{
    if (!on) { g_menu_hold_since[key] = 0; return; }
    if (!g_menu_hold_since[key]) { g_menu_hold_since[key] = g_x_tick; g_menu_next_rep[key] = g_x_tick + MENU_REPEAT_DELAY; ic_menu_key_pulse(key); return; }
    if (g_x_tick >= g_menu_next_rep[key]) { g_menu_next_rep[key] = g_x_tick + MENU_REPEAT_EVERY; ic_menu_key_pulse(key); }
}

static void ic_menu_release_all(void)
{
    for (int k = 0; k < MENU_KEYS; k++) { if (g_menu_release_tick[k]) { g_menu_release_tick[k] = 0; ic_sdl_key(k, 0); } g_menu_hold_since[k] = 0; }
    if (g_menu_mouse_down) { g_menu_mouse_down = 0; ic_sdl_mouse(0, 1); }
}

static void ic_menu_tick(void)
{
    if (!g_ic_want) return;
    for (int k = 0; k < MENU_KEYS; k++)
        if (g_menu_release_tick[k] && g_x_tick >= g_menu_release_tick[k]) { g_menu_release_tick[k] = 0; ic_sdl_key(k, 0); }

    /* is a menu (or the pause screen, main menu, loading screen) up? Then the game stopped calling CreateMove. */
    if (g_ic_calls != g_menu_prev_calls) { g_menu_prev_calls = g_ic_calls; g_menu_stalled = 0; }
    else if (g_menu_stalled < 1000000) g_menu_stalled++;
    const int active = g_ic_state != -1 && g_menu_stalled >= MENU_STALL;
    if (active != g_menu_active) {
        g_menu_active = active;
        if (!active) ic_menu_release_all();
        if (g_menu_logged_on++ < 6) bmvr_log("input: game menu control %s (CreateMove %s)", active ? "ON" : "off", active ? "stopped" : "running");
    }

    struct bmvr_xfer_input in;
    if (!bmvr_get_input(&in) || !(in.flags & BMVR_XFER_INPUT_FOCUSED)) { if (g_menu_mouse_down) { g_menu_mouse_down = 0; ic_sdl_mouse(0, 1); } return; }
    const unsigned L = in.buttons & 0xffu, R = (in.buttons >> 8) & 0xffu;
    const unsigned down = ((L | R) & BMVR_XFER_BTN_MENU) ? 1u : 0u;
    if (!g_menu_have_prev) { g_menu_have_prev = 1; g_menu_prev_down = down; return; }   /* a button already held at start is not a press */
    if (down && !g_menu_prev_down) {
        ic_menu_key_pulse(MK_ESC);
        if (g_menu_presses++ < 5) bmvr_log("input: Menu button -> Escape (game menu)");
    }
    g_menu_prev_down = down;
    if (!active) return;

    /* keys: left stick and d-pad */
    const float sx = in.stick[0][0], sy = in.stick[0][1];
    ic_menu_dir(MK_UP,    sy > 0.6f  || (L & BMVR_XFER_BTN_SECONDARY));
    ic_menu_dir(MK_DOWN,  sy < -0.6f || (L & BMVR_XFER_BTN_PRIMARY));
    ic_menu_dir(MK_LEFT,  sx < -0.6f);
    ic_menu_dir(MK_RIGHT, sx > 0.6f);
    {   static unsigned prev_ab;
        const unsigned ab = R & (BMVR_XFER_BTN_PRIMARY | BMVR_XFER_BTN_SECONDARY);
        const unsigned edge = ab & ~prev_ab;
        prev_ab = ab;
        if (edge & BMVR_XFER_BTN_PRIMARY) ic_menu_key_pulse(MK_ENTER);
        if (edge & BMVR_XFER_BTN_SECONDARY) ic_menu_key_pulse(MK_ESC);
    }

    /* mouse: right stick moves the cursor, a trigger clicks */
    const uint64_t n = now_ns();
    float dt = g_menu_last_ns && n >= g_menu_last_ns ? (float)((double)(n - g_menu_last_ns) / 1e9) : 0.011f;
    g_menu_last_ns = n;
    if (dt > 0.1f) dt = 0.011f;
    float cx = in.stick[1][0], cy = -in.stick[1][1];
    const float mag = sqrtf(cx * cx + cy * cy);
    int moved = 0;
    if (mag > 0.2f) {
        const float k = (mag - 0.2f) / 0.8f, speed = 1100.0f;              /* pixels per second at full deflection, with a gentle curve */
        g_menu_mx += cx / mag * k * k * speed * dt;
        g_menu_my += cy / mag * k * k * speed * dt;
        moved = 1;
    }
    const int trig = (g_menu_mouse_down ? 0.3f : 0.6f) < (in.trigger[0] > in.trigger[1] ? in.trigger[0] : in.trigger[1]);
    const int dn = trig && !g_menu_mouse_down, up = !trig && g_menu_mouse_down;
    g_menu_mouse_down = trig;
    if (moved || dn || up || g_menu_mx < 0.0f) ic_sdl_mouse(dn, up);
}

static void ic_status_tick(void)
{
    if (!g_ic_want || g_ic_state != 1 || g_x_tick == g_ic_hook_tick) return;
    if (g_ic_flag_on && g_ic_applied >= IC_FLAG_CLEAR_CALLS && g_ic_flag_path[0]) { unlink(g_ic_flag_path); g_ic_flag_on = 0; }
    if ((g_x_tick - g_ic_hook_tick) % 600) return;
    {   unsigned run = 0, ins = 0;
        for (int i = 0; i < g_ic_nh; i++) { if (g_ic_h[i].sane_run > run) run = g_ic_h[i].sane_run; ins += g_ic_h[i].insane; }
        bmvr_log("input: status tick %u: CreateMove calls %u, plausible-cmd run %u (need %d), implausible %u, armed %d%s, cmds written %u, no input yet %u",
                 g_x_tick, g_ic_calls, run, IC_LEARN, ins, g_ic_armed, g_ic_dead ? " (DEAD)" : "", g_ic_applied, g_ic_nocmd);
        if (g_ic_nh > 1) for (int i = 0; i < g_ic_nh; i++) bmvr_log("input:   hook %d %s: %u calls%s", i, g_ic_h[i].cls, g_ic_h[i].calls, g_ic_win == i ? " (CreateMove)" : "");
    }
    if (!g_ic_calls && g_x_tick - g_ic_hook_tick >= 1800) {
        static int warned;
        if (!warned) { warned = 1; bmvr_log("input: WARNING: the hooked slot has not been called once in %u frames. If a map is loaded this is the wrong class or input_slot= "
                                           "(compare the slot dump above); nothing was changed in the game.", g_x_tick - g_ic_hook_tick); }
    }
}

/* Called once per Present, after report_gl_once(), on the render thread (GL context current). */
static void xfer_tick(void)
{
    if (g_x_state < 0 || !g_gl_reported) return;
    g_x_tick++;
    /* v5: the frame delivered at this Present was rendered with the pose latched one Present ago; the next frame starts with the newest one. */
    g_x_pose_used = g_x_pose_pending;
    g_x_pose_pending = g_x_pose_latest.pose_id;
    g_x_pose_pending_data = g_x_pose_latest;
    load_cfg_once();
    if (!xfer_resolve_gl()) return;

    /* rt-trace: one full frame, twice per session. The frame is chosen by ACTIVITY (a gameplay frame makes
     * hundreds of SetRenderTarget(0) calls, a loading screen none), not by tick count: the first real-game
     * run traced two loading-screen frames and saw nothing. */
    g_setrt0_prev = g_setrt0_frame;
    g_setrt0_frame = 0;
    if (g_trace_active) {  /* the traced frame just ended */
        trace_flush();
        trace_summary();
        bmvr_log("rt-trace: end of traced frame (%u events traced, %d of %d trace slots unused)", g_trace_events, g_trace_left, TRACE_LINES);
        g_trace_left = 0;
        g_trace_active = 0;
        g_trace_frames_done++;
        g_trace_next_tick = g_x_tick + 360;
    } else if (g_trace_frames_done < 2 && g_x_tick >= g_trace_next_tick && g_setrt0_prev >= TRACE_MIN_BUSY) {
        bmvr_log("rt-trace: begin (tick %u; the previous frame made %u SetRenderTarget(0) calls; %u registry puts)", g_x_tick, g_setrt0_prev, g_reg_puts);
        g_trace_left = TRACE_LINES;
        g_trace_events = 0;
        g_trace_run = 0;
        g_trace_printed = 0;
        g_trace_ntab = 0;
        g_trace_tab_overflow = 0;
        g_trace_active = 1;
    }

    if (g_x_state == 0) {
        if (g_x_tick % XFER_RETRY != 1) return;
        if (!xfer_gl_usable() || !xfer_connect_and_import()) return;
        return; /* first snapshot on the next period */
    }
    st_try_install();
    st_status_tick();
    ic_try_install();
    ic_status_tick();
    ic_menu_tick();
    if (g_x_tick % g_x_every == 0) {
        if (g_st_sent) {
            g_st_sent = 0;   /* stage 4b: the stereo hook already delivered this frame's pair, mid-frame */
        } else if (xfer_gl_usable() && xfer_peer_alive()) {
            g_x_t0 = now_ns();
            if (g_spec[0].kind == SPEC_WINDOW) xfer_snapshot_window();
            else xfer_snapshot_target();
        }
        g_x_armed = 0;
    }
    if (g_x_state == 1 && g_x_tick % g_x_every == g_x_every - 1) {
        for (unsigned e = 0; e < XFER_EYES; e++) g_cap[e].valid = 0;
        g_cap_leaves = g_cap_mismatch = g_cap_unresolved = 0;   /* the miss line must describe THIS frame */
        g_x_armed = (g_spec[0].kind != SPEC_WINDOW);   /* record the targets left during the next frame */
    }
}

/* ------------------------------------------------------------------ counters */
static unsigned g_frames, g_tex_calls, g_rt_calls, g_setrt_calls, g_stretch_calls, g_getsurf_calls;
static uint64_t g_last_ns;

static uint64_t now_ns(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

/* ------------------------------------------------------------ constructor */
__attribute__((constructor)) static void bmvr_init(void)
{
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0) n = 0;
    exe[n] = 0;
    bmvr_log("bmvr_preload loaded, pid=%d exe=%s", getpid(), exe);
    bmvr_log("milestone 1 + stages 2b/3a/3b: all calls are forwarded to libtogl.so unchanged; the shim only reads GL state and, if the helper runs, copies one render target (sync: glFinish+ack, or GPU semaphores with sync=sem).");
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
    xfer_tick();
    g_frames++;
    uint64_t t = now_ns();
    if (!g_last_ns) g_last_ns = t;
    if (t - g_last_ns >= 5000000000ull) {
        bmvr_log("stats: %.1f fps | calls in last window: CreateTexture=%u CreateRenderTarget=%u SetRenderTarget=%u StretchRect=%u "
                 "surfLevel=%u | registry puts=%u (%u from GetSurfaceLevel; cache holds %d), pinned=%u (%u evicted)",
             (double)g_frames * 1e9 / (double)(t - g_last_ns), g_tex_calls, g_rt_calls, g_setrt_calls, g_stretch_calls,
             g_getsurf_calls, g_reg_puts, g_reg_surf_puts, REG_SIZE, g_pin_puts, g_pin_evicted);
        g_frames = 1; g_tex_calls = g_rt_calls = g_setrt_calls = g_stretch_calls = g_getsurf_calls = 0; g_last_ns = t;
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
    if (idx == 0) { g_setrt0_frame++; rt_leave(g_cur_rt0, surf); }   /* stage 2b: GL still shows the target being left */
    HRESULT hr = real ? real(self, idx, surf) : -1;
    if (idx == 0 && hr >= 0) g_cur_rt0 = surf;
    return hr;
}

typedef HRESULT (*PFN_GetSurfLevel)(void *, unsigned, void **);
BMVR_EXPORT HRESULT bmvr_GetSurfaceLevel(void *self, unsigned level, void **ppSurf) __asm__(SYM_GETSURF);
BMVR_EXPORT HRESULT bmvr_GetSurfaceLevel(void *self, unsigned level, void **ppSurf)
{
    static PFN_GetSurfLevel real;
    if (!real) real = (PFN_GetSurfLevel)real_sym(SYM_GETSURF);
    g_getsurf_calls++;
    g_getsurf_total++;
    HRESULT hr = real ? real(self, level, ppSurf) : -1;
    /* Level 0 of a registered texture: the surface inherits its label and size. */
    if (hr >= 0 && level == 0 && ppSurf && *ppSurf) {
        struct reg_ent e;
        if (reg_get(self, &e)) reg_put(*ppSurf, e.w, e.h, e.fmt, e.usage, e.label, 1);
    }
    return hr;
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
    if (hr >= 0 && ppTex && *ppTex && (usage & 1u)) reg_put_pinned(*ppTex, w, h, fmt, usage, label);  /* stage 2b registry (pinned) */
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
    if (hr >= 0 && ppSurf && *ppSurf) reg_put_pinned(*ppSurf, w, h, fmt, 1u, label);  /* stage 2b registry (pinned) */
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
