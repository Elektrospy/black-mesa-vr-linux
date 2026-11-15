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
#include <sys/time.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "bmvr_xfer.h"

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

/* ============================================================ stage 2a: GPU hand-off test
 * Optional and passive: nothing happens unless the 64-bit helper (bmvr_xfer_helper) is running
 * and has created "<game folder>/bmvr_xfer.sock". Then, once per XFER_EVERY presents, the frame
 * about to be presented is blitted into a texture backed by the helper's Vulkan memory. The helper
 * writes it to a PPM file. See linux/bmvr_xfer.h for the protocol.
 *
 * Safety: if anything fails the shim logs "xfer: ..." and goes back to doing nothing; the game is
 * never blocked for longer than the 2 s socket timeout, and GL state is saved/restored around
 * every use. */
#define XFER_EVERY 120          /* presents between snapshots (~2 s at 60 fps) */
#define XFER_RETRY 240          /* presents between attempts to find the helper */
#define XFER_TIMEOUT_SEC 2

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
typedef unsigned (*PFN_glCheckFramebufferStatus)(unsigned);
typedef void (*PFN_glBlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
typedef void (*PFN_glFinish)(void);
typedef unsigned (*PFN_glGetError)(void);
typedef unsigned char (*PFN_glIsEnabled)(unsigned);
typedef void (*PFN_glEnable)(unsigned);
typedef void (*PFN_glDisable)(unsigned);

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
    PFN_glCheckFramebufferStatus CheckFramebufferStatus;
    PFN_glBlitFramebuffer BlitFramebuffer;
    PFN_glFinish Finish;
    PFN_glGetError GetError;
    PFN_glIsEnabled IsEnabled;
    PFN_glEnable Enable;
    PFN_glDisable Disable;
    PFN_glGetIntegerv GetIntegerv;
} xgl;

static int g_x_state;            /* 0 = looking for the helper, 1 = connected, -1 = given up */
static int g_x_sock = -1;
static unsigned g_x_tick;
static unsigned g_x_snapshots;
static uint32_t g_x_w, g_x_h;
static unsigned g_x_mem, g_x_tex, g_x_fbo;

static int xfer_resolve_gl(void)
{
    static int tried, ok;
    if (tried) return ok;
    tried = 1;
    PFN_SDL_GL_GetProcAddress gp = g_getproc;
    if (!gp) { tried = 0; return 0; }
#define R(field, name) xgl.field = (void *)gp(name); if (!xgl.field) { bmvr_log("xfer: GL entry point %s missing", name); return 0; }
    R(CreateMemoryObjects, "glCreateMemoryObjectsEXT") R(DeleteMemoryObjects, "glDeleteMemoryObjectsEXT")
    R(MemoryObjectParameteriv, "glMemoryObjectParameterivEXT") R(ImportMemoryFd, "glImportMemoryFdEXT")
    R(TexStorageMem2D, "glTexStorageMem2DEXT") R(GenTextures, "glGenTextures") R(DeleteTextures, "glDeleteTextures")
    R(BindTexture, "glBindTexture") R(TexParameteri, "glTexParameteri") R(GenFramebuffers, "glGenFramebuffers")
    R(DeleteFramebuffers, "glDeleteFramebuffers") R(BindFramebuffer, "glBindFramebuffer")
    R(FramebufferTexture2D, "glFramebufferTexture2D") R(CheckFramebufferStatus, "glCheckFramebufferStatus")
    R(BlitFramebuffer, "glBlitFramebuffer") R(Finish, "glFinish") R(GetError, "glGetError") R(IsEnabled, "glIsEnabled")
    R(Enable, "glEnable") R(Disable, "glDisable") R(GetIntegerv, "glGetIntegerv")
#undef R
    ok = 1;
    return 1;
}

static void xfer_teardown(void)
{
    if (g_x_sock >= 0) { close(g_x_sock); g_x_sock = -1; }
    if (xgl.DeleteFramebuffers && g_x_fbo) xgl.DeleteFramebuffers(1, &g_x_fbo);
    if (xgl.DeleteTextures && g_x_tex) xgl.DeleteTextures(1, &g_x_tex);
    if (xgl.DeleteMemoryObjects && g_x_mem) xgl.DeleteMemoryObjects(1, &g_x_mem);
    g_x_fbo = g_x_tex = g_x_mem = 0;
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
    char exe[400];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) return 0;
    *slash = 0;
    snprintf(path, sizeof path, "%s/%s", exe, BMVR_XFER_SOCKET_NAME);
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
    char ctl[CMSG_SPACE(sizeof(int))];
    memset(ctl, 0, sizeof ctl);
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = sizeof ctl;
    ssize_t got = recvmsg(s, &m, 0);
    int fd = -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof fd);
    if (got != (ssize_t)sizeof h || fd < 0 || h.magic != BMVR_XFER_MAGIC_HELLO || h.version != BMVR_XFER_VERSION ||
        !h.width || !h.height || h.width > 8192 || h.height > 8192) {
        bmvr_log("xfer: bad hello from helper (got %d bytes, fd %d)", (int)got, fd);
        if (fd >= 0) close(fd);
        close(s);
        return 0;
    }
    bmvr_log("xfer: helper connected: shared image %ux%u, memory %llu bytes, fd %d", h.width, h.height,
             (unsigned long long)h.memory_size, fd);

    if (!xfer_resolve_gl()) { close(fd); close(s); g_x_state = -1; return 0; }
    while (xgl.GetError()) { /* drain errors left by the game */ }

    int old_tex = 0;
    xgl.GetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);

    int one = 1, ok = 1;
    xgl.CreateMemoryObjects(1, &g_x_mem);
    ok &= xfer_gl_ok("glCreateMemoryObjectsEXT");
    if (h.dedicated) { xgl.MemoryObjectParameteriv(g_x_mem, GL_DEDICATED_MEMORY_OBJECT_EXT, &one); ok &= xfer_gl_ok("dedicated flag"); }
    xgl.ImportMemoryFd(g_x_mem, h.memory_size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd); /* on success GL owns the fd */
    int imp = xfer_gl_ok("glImportMemoryFdEXT");
    ok &= imp;
    if (!imp) close(fd);
    xgl.GenTextures(1, &g_x_tex);
    xgl.BindTexture(GL_TEXTURE_2D, g_x_tex);
    if (h.optimal_tiling) xgl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, (int)GL_OPTIMAL_TILING_EXT);
    xgl.TexStorageMem2D(GL_TEXTURE_2D, 1, h.gl_internal_format, (int)h.width, (int)h.height, g_x_mem, 0);
    ok &= xfer_gl_ok("glTexStorageMem2DEXT");
    xgl.BindTexture(GL_TEXTURE_2D, (unsigned)old_tex);

    if (ok) {
        int old_draw = 0, old_read = 0;
        xgl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
        xgl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
        xgl.GenFramebuffers(1, &g_x_fbo);
        xgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_x_fbo);
        xgl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_x_tex, 0);
        unsigned st = xgl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        xgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)old_draw);
        xgl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)old_read);
        if (st != GL_FRAMEBUFFER_COMPLETE) { bmvr_log("xfer: framebuffer on the imported texture is incomplete (0x%x)", st); ok = 0; }
        ok &= xfer_gl_ok("framebuffer setup");
    }
    if (!ok) {
        bmvr_log("xfer: import failed; stage 2a disabled for this session");
        xfer_teardown();
        close(s);
        g_x_state = -1;
        return 0;
    }
    g_x_w = h.width;
    g_x_h = h.height;
    g_x_sock = s;
    g_x_state = 1;
    bmvr_log("xfer: imported the helper's Vulkan memory as GL texture %u; snapshots every %d presents", g_x_tex, XFER_EVERY);
    return 1;
}

/* Blit the frame about to be presented into the shared texture, then tell the helper. */
static void xfer_snapshot(void)
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
    xgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_x_fbo);
    /* Destination Y range is flipped: GL rows start at the bottom, the Vulkan image's row 0 is the top. */
    xgl.BlitFramebuffer(0, 0, sw, sh, 0, (int)g_x_h, (int)g_x_w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    int blit_ok = xfer_gl_ok("glBlitFramebuffer");
    xgl.Finish(); /* test only: the real renderer will use GL_EXT_semaphore_fd instead of a full stall */
    xgl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)old_read);
    xgl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)old_draw);
    if (scissor) xgl.Enable(GL_SCISSOR_TEST);
    if (!blit_ok) return;

    struct bmvr_xfer_frame fr;
    fr.magic = BMVR_XFER_MAGIC_FRAME;
    fr.tick = g_x_tick;
    fr.src_w = (uint32_t)sw;
    fr.src_h = (uint32_t)sh;
    struct bmvr_xfer_ack ack;
    if (send(g_x_sock, &fr, sizeof fr, MSG_NOSIGNAL) != (ssize_t)sizeof fr ||
        recv(g_x_sock, &ack, sizeof ack, MSG_WAITALL) != (ssize_t)sizeof ack || ack.magic != BMVR_XFER_MAGIC_ACK) {
        bmvr_log("xfer: lost the helper (no ack); back to searching");
        xfer_teardown();
        g_x_state = 0;
        return;
    }
    if (g_x_snapshots++ < 3) bmvr_log("xfer: snapshot %u delivered (tick %u, %dx%d -> %ux%u)", g_x_snapshots, g_x_tick, sw, sh, g_x_w, g_x_h);
}

/* Called once per Present, after report_gl_once(), on the render thread (GL context current). */
static void xfer_tick(void)
{
    if (g_x_state < 0 || !g_gl_reported) return;
    g_x_tick++;
    if (g_x_state == 0) {
        if (g_x_tick % XFER_RETRY != 1) return;
        if (!xfer_resolve_gl() || !xfer_connect_and_import()) return;
        return; /* first snapshot on the next period */
    }
    if (g_x_tick % XFER_EVERY == 0) xfer_snapshot();
}

/* ------------------------------------------------------------------ counters */
static unsigned g_frames, g_tex_calls, g_rt_calls, g_setrt_calls, g_stretch_calls;
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
    xfer_tick();
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
