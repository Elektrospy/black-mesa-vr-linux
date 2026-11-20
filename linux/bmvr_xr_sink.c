/*
 * Black Mesa VR - native Linux port, stage 4a: OpenXR sink (milestones X0: probe, X1: frame-loop self-test). See
 * bmvr_xr_sink.h and docs/LINUX_OPENXR_CONSUMER.md.
 *
 * Both libraries are loaded with dlopen (libopenxr_loader.so.1, libvulkan.so.1), like the helper already does for
 * Vulkan, so the helper still builds and runs on a machine without OpenXR. Only HEADERS are needed to build: the Khronos
 * OpenXR headers vendored in third_party/openxr/include and the Vulkan headers (see scripts/build-linux.sh).
 *
 * Order of operations follows XR_KHR_vulkan_enable2 (the runtime, not the application, chooses the physical device):
 *   xrCreateInstance -> xrGetSystem -> xrGetVulkanGraphicsRequirements2KHR -> xrCreateVulkanInstanceKHR ->
 *   xrGetVulkanGraphicsDevice2KHR -> xrCreateVulkanDeviceKHR -> xrCreateSession -> swapchain.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bmvr_xr_sink.h"

#define XLOG(...) do { fprintf(stderr, "[xr] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ------------------------------------------------------------------ result ledger */
/* Every requirement is recorded with a verdict, so the end of the probe is a table that can be pasted into a bug report
 * or into the design document. REQUIRED items decide the exit code; INFO items are facts to record. */
#define LEDGER_MAX 48
static struct { const char *name; int ok; int required; char detail[160]; } g_ledger[LEDGER_MAX];
static int g_ledger_n;

static void ledger(const char *name, int ok, int required, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void ledger(const char *name, int ok, int required, const char *fmt, ...)
{
    if (g_ledger_n >= LEDGER_MAX) return;
    va_list ap;
    va_start(ap, fmt);
    g_ledger[g_ledger_n].name = name;
    g_ledger[g_ledger_n].ok = ok;
    g_ledger[g_ledger_n].required = required;
    vsnprintf(g_ledger[g_ledger_n].detail, sizeof g_ledger[g_ledger_n].detail, fmt, ap);
    va_end(ap);
    XLOG("%s %s: %s", ok ? "ok  " : (required ? "FAIL" : "note"), name, g_ledger[g_ledger_n].detail);
    g_ledger_n++;
}

/* ------------------------------------------------------------------ loaders */
static PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_;

#define XR_INSTANCE_FUNCS(X) \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) \
    X(xrEnumerateViewConfigurations) X(xrEnumerateViewConfigurationViews) X(xrEnumerateEnvironmentBlendModes) \
    X(xrCreateSession) X(xrDestroySession) X(xrPollEvent) X(xrResultToString) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) \
    X(xrGetVulkanGraphicsRequirements2KHR) X(xrCreateVulkanInstanceKHR) X(xrGetVulkanGraphicsDevice2KHR) \
    X(xrCreateVulkanDeviceKHR)

#define VK_INST_FUNCS(X) \
    X(vkDestroyInstance) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceImageFormatProperties2) \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties) X(vkGetDeviceProcAddr)

#define DECL(name) static PFN_##name name##_;
XR_INSTANCE_FUNCS(DECL)
VK_INST_FUNCS(DECL)
#undef DECL
static PFN_vkDestroyDevice vkDestroyDevice_;

static const char *xr_rs(XrInstance inst, XrResult r)
{
    static char buf[2][XR_MAX_RESULT_STRING_SIZE + 16];
    static int flip;
    char *b = buf[flip ^= 1];
    if (inst != XR_NULL_HANDLE && xrResultToString_ && XR_SUCCEEDED(xrResultToString_(inst, r, b))) return b;
    snprintf(b, sizeof buf[0], "XrResult %d", (int)r);
    return b;
}

/* ------------------------------------------------------------------ small tables */
static const char *vk_format_name(int64_t f)
{
    switch (f) {
    case 37: return "R8G8B8A8_UNORM";
    case 43: return "R8G8B8A8_SRGB";
    case 44: return "B8G8R8A8_UNORM";
    case 50: return "B8G8R8A8_SRGB";
    case 64: return "A2B10G10R10_UNORM_PACK32";
    case 97: return "R16G16B16A16_SFLOAT";
    case 124: return "D16_UNORM";
    case 126: return "D32_SFLOAT";
    case 129: return "D24_UNORM_S8_UINT";
    case 130: return "D32_SFLOAT_S8_UINT";
    default: return "?";
    }
}

static const char *session_state_name(XrSessionState s)
{
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

static void print_uuid(const char *label, const uint8_t u[VK_UUID_SIZE])
{
    char s[VK_UUID_SIZE * 2 + 1];
    for (unsigned i = 0; i < VK_UUID_SIZE; i++) snprintf(s + i * 2, 3, "%02x", u[i]);
    XLOG("%s%s", label, s);
}

/* OpenXR versions are 16/16/32 bits, Vulkan's are 7/10/12 (variant, major, minor, patch). Compare major.minor only. */
static int xr_ver_to_vk_minor_key(XrVersion v) { return (int)(XR_VERSION_MAJOR(v) * 1000 + XR_VERSION_MINOR(v)); }
static int vk_ver_minor_key(uint32_t v) { return (int)(VK_API_VERSION_MAJOR(v) * 1000 + VK_API_VERSION_MINOR(v)); }

/* ------------------------------------------------------------------ the probe */
struct ctx {
    void *xr_lib, *vk_lib;
    XrInstance inst;
    XrSystemId sys;
    VkInstance vk_inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    XrSession sess;
    XrSwapchain sc;
};

static void ctx_close(struct ctx *c)
{
    if (c->sc) xrDestroySwapchain_(c->sc);
    if (c->sess) xrDestroySession_(c->sess);
    if (c->dev && vkDestroyDevice_) vkDestroyDevice_(c->dev, NULL);
    if (c->vk_inst && vkDestroyInstance_) vkDestroyInstance_(c->vk_inst, NULL);
    if (c->inst) xrDestroyInstance_(c->inst);
    /* The libraries stay loaded: unloading a runtime that owns threads is not worth the risk for a process that exits. */
}

static int load_xr_loader(struct ctx *c)
{
    const char *names[] = { "libopenxr_loader.so.1", "libopenxr_loader.so" };
    for (unsigned i = 0; i < sizeof names / sizeof *names && !c->xr_lib; i++) c->xr_lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!c->xr_lib) {
        XLOG("cannot load the OpenXR loader (libopenxr_loader.so.1): %s", dlerror());
        XLOG("install it (Debian/Ubuntu: libopenxr-loader1; Arch/CachyOS: openxr) and an OpenXR runtime (SteamVR or Monado)");
        return 0;
    }
    xrGetInstanceProcAddr_ = (PFN_xrGetInstanceProcAddr)dlsym(c->xr_lib, "xrGetInstanceProcAddr");
    return xrGetInstanceProcAddr_ != NULL;
}

static void print_wrapped(const char *prefix, char **items, uint32_t n)
{
    char line[256];
    int len = 0;
    line[0] = 0;
    for (uint32_t i = 0; i < n; i++) {
        int add = (int)strlen(items[i]) + 1;
        if (len + add > 110 && len) { XLOG("%s%s", prefix, line); line[0] = 0; len = 0; }
        len += snprintf(line + len, sizeof line - (size_t)len, "%s ", items[i]);
    }
    if (len) XLOG("%s%s", prefix, line);
}

static int run_probe(struct ctx *c, const struct bmvr_xr_probe_opts *o, int *unavailable)
{
    XrResult r;
    int failed = 0;

    if (!load_xr_loader(c)) { *unavailable = 1; return 0; }

    /* --- instance extensions the runtime offers */
    PFN_xrEnumerateInstanceExtensionProperties enumExt = NULL;
    PFN_xrCreateInstance createInst = NULL;
    xrGetInstanceProcAddr_(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enumExt);
    xrGetInstanceProcAddr_(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&createInst);
    if (!enumExt || !createInst) { XLOG("loader does not expose xrCreateInstance"); *unavailable = 1; return 0; }

    uint32_t next = 0;
    r = enumExt(NULL, 0, &next, NULL);
    if (XR_FAILED(r)) {
        XLOG("xrEnumerateInstanceExtensionProperties failed (%d): no OpenXR runtime installed or active?", (int)r);
        XLOG("check the active runtime (XR_RUNTIME_JSON, ~/.config/openxr/1/active_runtime.json, /etc/xdg/openxr/1/active_runtime.json, SteamVR/Monado settings)");
        XLOG("SteamVR: running is not enough, it must be the active runtime: ln -sf <SteamVR dir>/steamxr_linux64.json ~/.config/openxr/1/active_runtime.json");
        *unavailable = 1;
        return 0;
    }
    XrExtensionProperties *ext = calloc(next ? next : 1, sizeof *ext);
    char **extnames = calloc(next ? next : 1, sizeof *extnames);
    for (uint32_t i = 0; i < next; i++) ext[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    if (XR_FAILED(enumExt(NULL, next, &next, ext))) next = 0;
    int has_enable2 = 0, has_depth = 0;
    for (uint32_t i = 0; i < next; i++) {
        extnames[i] = ext[i].extensionName;
        if (!strcmp(ext[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) has_enable2 = 1;
        if (!strcmp(ext[i].extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)) has_depth = 1;
    }
    XLOG("runtime offers %u instance extensions:", next);
    print_wrapped("    ", extnames, next);
    free(extnames);
    free(ext);
    ledger("XR_KHR_vulkan_enable2", has_enable2, 1, has_enable2 ? "offered" : "NOT offered by this runtime");
    ledger("XR_KHR_composition_layer_depth", has_depth, 0, has_depth ? "offered (depth layer possible later)" : "not offered");
    if (!has_enable2) return 1;

    /* --- instance */
    const char *want_ext[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { .type = XR_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = 1, .enabledExtensionNames = want_ext };
    snprintf(ici.applicationInfo.applicationName, sizeof ici.applicationInfo.applicationName, "bmvr_xfer_helper");
    ici.applicationInfo.applicationVersion = 1;
    snprintf(ici.applicationInfo.engineName, sizeof ici.applicationInfo.engineName, "bmvr");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0; /* 1.0 works with every 1.x runtime/loader */
    r = createInst(&ici, &c->inst);
    if (r == XR_ERROR_RUNTIME_UNAVAILABLE || r == XR_ERROR_RUNTIME_FAILURE) {
        /* Typical cause: the runtime's library is installed but its service/compositor is not running. */
        XLOG("xrCreateInstance failed (%d): the runtime is installed but not usable right now", (int)r);
        XLOG("Monado: start monado-service first. SteamVR: start SteamVR (and set it as the active OpenXR runtime).");
        *unavailable = 1;
        return 0;
    }
    if (XR_FAILED(r)) { ledger("xrCreateInstance", 0, 1, "%s", xr_rs(XR_NULL_HANDLE, r)); return 1; }
#define LOADX(name) xrGetInstanceProcAddr_(c->inst, #name, (PFN_xrVoidFunction *)&name##_); \
    if (!name##_) { ledger("load " #name, 0, 1, "missing in this runtime/loader"); return 1; }
    XR_INSTANCE_FUNCS(LOADX)
#undef LOADX

    XrInstanceProperties ip = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties_(c->inst, &ip);
    ledger("xrCreateInstance", 1, 1, "runtime \"%s\" %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
           XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    /* --- system (headset) */
    XrSystemGetInfo sgi = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    r = xrGetSystem_(c->inst, &sgi, &c->sys);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        XLOG("the runtime is there but reports no head-mounted display (headset off / not connected, or a simulated driver is not enabled)");
        *unavailable = 1;
        return 0;
    }
    if (XR_FAILED(r)) { ledger("xrGetSystem", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }
    XrSystemProperties sp = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties_(c->inst, c->sys, &sp);
    ledger("xrGetSystem", 1, 1, "\"%s\" vendor %u, tracking: orientation=%d position=%d, max swapchain %ux%u, layers %u",
           sp.systemName, sp.vendorId, (int)sp.trackingProperties.orientationTracking, (int)sp.trackingProperties.positionTracking,
           sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount);

    /* --- view configuration: what size should the helper's images be? */
    uint32_t nvc = 0;
    xrEnumerateViewConfigurations_(c->inst, c->sys, 0, &nvc, NULL);
    XrViewConfigurationType vcs[8];
    if (nvc > 8) nvc = 8;
    xrEnumerateViewConfigurations_(c->inst, c->sys, nvc, &nvc, vcs);
    int has_stereo = 0;
    for (uint32_t i = 0; i < nvc; i++) if (vcs[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) has_stereo = 1;
    ledger("PRIMARY_STEREO view configuration", has_stereo, 1, "%u configuration(s) offered", nvc);
    if (!has_stereo) return 1;
    XrViewConfigurationView views[4];
    uint32_t nv = 0;
    for (int i = 0; i < 4; i++) views[i] = (XrViewConfigurationView){ .type = XR_TYPE_VIEW_CONFIGURATION_VIEW };
    r = xrEnumerateViewConfigurationViews_(c->inst, c->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 4, &nv, views);
    if (XR_FAILED(r) || nv != 2) { ledger("view sizes", 0, 1, "%s, %u views", xr_rs(c->inst, r), nv); return 1; }
    ledger("view sizes", 1, 1, "recommended %ux%u per eye (max %ux%u), samples recommended %u; left==right: %s",
           views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight, views[0].maxImageRectWidth,
           views[0].maxImageRectHeight, views[0].recommendedSwapchainSampleCount,
           (views[0].recommendedImageRectWidth == views[1].recommendedImageRectWidth &&
            views[0].recommendedImageRectHeight == views[1].recommendedImageRectHeight) ? "yes" : "NO");

    XrEnvironmentBlendMode bm[4];
    uint32_t nbm = 0;
    xrEnumerateEnvironmentBlendModes_(c->inst, c->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 4, &nbm, bm);
    {
        int opaque = 0;
        for (uint32_t i = 0; i < nbm; i++) if (bm[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) opaque = 1;
        ledger("blend mode OPAQUE", opaque, 0, "%u mode(s)", nbm);
    }

    /* --- Vulkan, runtime-owned (XR_KHR_vulkan_enable2) */
    XrGraphicsRequirementsVulkanKHR req = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    r = xrGetVulkanGraphicsRequirements2KHR_(c->inst, c->sys, &req);
    if (XR_FAILED(r)) { ledger("Vulkan requirements", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }
    if (XR_VERSION_MAJOR(req.maxApiVersionSupported) >= 1023) { /* Monado reports 1023.1023 = "no upper limit" */
        ledger("Vulkan requirements", 1, 1, "runtime accepts Vulkan %u.%u and newer", XR_VERSION_MAJOR(req.minApiVersionSupported),
               XR_VERSION_MINOR(req.minApiVersionSupported));
    } else {
        ledger("Vulkan requirements", 1, 1, "runtime accepts Vulkan %u.%u .. %u.%u", XR_VERSION_MAJOR(req.minApiVersionSupported),
               XR_VERSION_MINOR(req.minApiVersionSupported), XR_VERSION_MAJOR(req.maxApiVersionSupported),
               XR_VERSION_MINOR(req.maxApiVersionSupported));
    }

    c->vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!c->vk_lib) { ledger("libvulkan.so.1", 0, 1, "%s", dlerror()); return 1; }
    vkGetInstanceProcAddr_ = (PFN_vkGetInstanceProcAddr)dlsym(c->vk_lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr_) { ledger("libvulkan.so.1", 0, 1, "vkGetInstanceProcAddr missing"); return 1; }

    /* Vulkan 1.1 is the floor the helper needs (external memory/semaphore capabilities are core there). */
    uint32_t vk_api = VK_API_VERSION_1_1;
    if (xr_ver_to_vk_minor_key(req.minApiVersionSupported) > vk_ver_minor_key(vk_api)) {
        vk_api = VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported), 0);
    }
    if (xr_ver_to_vk_minor_key(req.maxApiVersionSupported) < vk_ver_minor_key(vk_api)) {
        ledger("Vulkan version window", 0, 1, "runtime maximum is below Vulkan 1.1");
        return 1;
    }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "bmvr_xfer_helper", .apiVersion = vk_api };
    VkInstanceCreateInfo vici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    XrVulkanInstanceCreateInfoKHR xvici = { .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = c->sys,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr_, .vulkanCreateInfo = &vici };
    VkResult vr = VK_SUCCESS;
    r = xrCreateVulkanInstanceKHR_(c->inst, &xvici, &c->vk_inst, &vr);
    if (XR_FAILED(r) || vr != VK_SUCCESS) { ledger("xrCreateVulkanInstanceKHR", 0, 1, "%s, VkResult %d", xr_rs(c->inst, r), (int)vr); return 1; }
    ledger("xrCreateVulkanInstanceKHR", 1, 1, "instance created with API %u.%u", VK_API_VERSION_MAJOR(vk_api), VK_API_VERSION_MINOR(vk_api));
#define LOADV(name) name##_ = (PFN_##name)vkGetInstanceProcAddr_(c->vk_inst, #name); \
    if (!name##_) { ledger("load " #name, 0, 1, "missing"); return 1; }
    VK_INST_FUNCS(LOADV)
#undef LOADV

    XrVulkanGraphicsDeviceGetInfoKHR gdi = { .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = c->sys, .vulkanInstance = c->vk_inst };
    r = xrGetVulkanGraphicsDevice2KHR_(c->inst, &gdi, &c->phys);
    if (XR_FAILED(r)) { ledger("xrGetVulkanGraphicsDevice2KHR", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }

    VkPhysicalDeviceIDProperties idp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &idp };
    vkGetPhysicalDeviceProperties2_(c->phys, &pp);
    ledger("runtime-chosen GPU", 1, 1, "\"%s\" (Vulkan %u.%u)", pp.properties.deviceName, VK_API_VERSION_MAJOR(pp.properties.apiVersion),
           VK_API_VERSION_MINOR(pp.properties.apiVersion));
    print_uuid("  device UUID (compare with the game's GL device UUID): ", idp.deviceUUID);

    /* The design premise: the helper's exportable images must live on THIS device. Check what the helper's own startup checks. */
    {
        VkPhysicalDeviceExternalImageFormatInfo ei = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        VkPhysicalDeviceImageFormatInfo2 fi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &ei,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
        VkExternalImageFormatProperties eo = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
        VkImageFormatProperties2 fo = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &eo };
        VkResult fr = vkGetPhysicalDeviceImageFormatProperties2_(c->phys, &fi, &fo);
        int exp = fr == VK_SUCCESS && (eo.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
        ledger("exportable R8G8B8A8 image on this GPU", exp, 1, "%s (opaque-fd, optimal tiling, helper usage flags)",
               exp ? "yes" : "NO: the shim could not import the helper's images");
        VkPhysicalDeviceExternalSemaphoreInfo si = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
        VkExternalSemaphoreProperties so = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
        vkGetPhysicalDeviceExternalSemaphoreProperties_(c->phys, &si, &so);
        int sem = (so.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) &&
                  (so.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
        ledger("opaque-fd semaphores on this GPU", sem, 0, sem ? "exportable and importable (sync=sem possible)" : "not both: sync=sem would fall back to glFinish");
    }

    uint32_t nde = 0;
    vkEnumerateDeviceExtensionProperties_(c->phys, NULL, &nde, NULL);
    VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof *de);
    vkEnumerateDeviceExtensionProperties_(c->phys, NULL, &nde, de);
    int has_memfd = 0, has_semfd = 0;
    for (uint32_t i = 0; i < nde; i++) {
        if (!strcmp(de[i].extensionName, "VK_KHR_external_memory_fd")) has_memfd = 1;
        if (!strcmp(de[i].extensionName, "VK_KHR_external_semaphore_fd")) has_semfd = 1;
    }
    free(de);
    ledger("VK_KHR_external_memory_fd (device)", has_memfd, 1, has_memfd ? "present" : "missing");
    ledger("VK_KHR_external_semaphore_fd (device)", has_semfd, 0, has_semfd ? "present" : "missing (sync=sem unavailable)");
    if (!has_memfd) return 1;

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties_(c->phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties_(c->phys, &nq, qf);
    int qfam = -1;
    for (uint32_t i = 0; i < nq; i++) if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qfam < 0) qfam = (int)i;
    if (qfam < 0) { free(qf); ledger("graphics queue family", 0, 1, "none"); return 1; }
    uint32_t qcount = qf[qfam].queueCount;
    free(qf);
    ledger("graphics queue family", 1, 1, "family %d with %u queue(s) (the design starts with one queue + a mutex)", qfam, qcount);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = (uint32_t)qfam, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dext[2] = { "VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = has_semfd ? 2u : 1u, .ppEnabledExtensionNames = dext };
    XrVulkanDeviceCreateInfoKHR xvdi = { .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = c->sys, .pfnGetInstanceProcAddr = vkGetInstanceProcAddr_,
        .vulkanPhysicalDevice = c->phys, .vulkanCreateInfo = &dci };
    vr = VK_SUCCESS;
    r = xrCreateVulkanDeviceKHR_(c->inst, &xvdi, &c->dev, &vr);
    if (XR_FAILED(r) || vr != VK_SUCCESS) { ledger("xrCreateVulkanDeviceKHR", 0, 1, "%s, VkResult %d", xr_rs(c->inst, r), (int)vr); return 1; }
    ledger("xrCreateVulkanDeviceKHR", 1, 1, "device created with %s", has_semfd ? "external_memory_fd + external_semaphore_fd" : "external_memory_fd");
    {
        PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr_;
        vkDestroyDevice_ = (PFN_vkDestroyDevice)gdpa(c->dev, "vkDestroyDevice");
    }

    /* --- session */
    XrGraphicsBindingVulkanKHR gb = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = c->vk_inst, .physicalDevice = c->phys,
        .device = c->dev, .queueFamilyIndex = (uint32_t)qfam, .queueIndex = 0 };
    XrSessionCreateInfo sci = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &gb, .systemId = c->sys };
    r = xrCreateSession_(c->inst, &sci, &c->sess);
    if (XR_FAILED(r)) { ledger("xrCreateSession", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }
    ledger("xrCreateSession", 1, 1, "created on the runtime-chosen device");

    /* --- swapchain formats and one real swapchain (the sRGB/RGBA assumption is the biggest "might not hold") */
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats_(c->sess, 0, &nf, NULL);
    int64_t *fmts = calloc(nf ? nf : 1, sizeof *fmts);
    xrEnumerateSwapchainFormats_(c->sess, nf, &nf, fmts);
    {
        char line[400];
        int n = 0, has_srgb_rgba = 0, has_unorm_rgba = 0;
        for (uint32_t i = 0; i < nf; i++) {
            if (fmts[i] == 43) has_srgb_rgba = 1;
            if (fmts[i] == 37) has_unorm_rgba = 1;
            n += snprintf(line + n, sizeof line - (size_t)n, "%s%lld(%s)", i ? ", " : "", (long long)fmts[i], vk_format_name(fmts[i]));
            if (n > (int)sizeof line - 40) break;
        }
        XLOG("swapchain formats (%u): %s", nf, line);
        ledger("swapchain format R8G8B8A8_SRGB", has_srgb_rgba, 1,
               has_srgb_rgba ? "offered: the raw-copy design works" : "NOT offered: needs the shader fallback (design doc, Decision 3)");
        ledger("swapchain format R8G8B8A8_UNORM", has_unorm_rgba, 0, has_unorm_rgba ? "offered" : "not offered");
    }
    free(fmts);

    uint32_t sw = o && o->swapchain_w > 0 ? (uint32_t)o->swapchain_w : views[0].recommendedImageRectWidth;
    uint32_t sh = o && o->swapchain_h > 0 ? (uint32_t)o->swapchain_h : views[0].recommendedImageRectHeight;
    XrSwapchainCreateInfo sc = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
        .format = 43, .sampleCount = 1, .width = sw, .height = sh, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
    r = xrCreateSwapchain_(c->sess, &sc, &c->sc);
    if (XR_FAILED(r)) {
        ledger("xrCreateSwapchain", 0, 1, "%ux%u R8G8B8A8_SRGB transfer-dst: %s", sw, sh, xr_rs(c->inst, r));
        failed = 1;
    } else {
        uint32_t ni = 0;
        xrEnumerateSwapchainImages_(c->sc, 0, &ni, NULL);
        ledger("xrCreateSwapchain", 1, 1, "%ux%u R8G8B8A8_SRGB, usage TRANSFER_DST|SAMPLED, %u image(s)", sw, sh, ni);
    }

    /* --- session state: wait for READY (no frames are submitted by the probe) */
    if (o && o->wait_session_ms > 0) {
        int ready = 0;
        struct timespec t0, t;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            XrEventDataBuffer ev = { .type = XR_TYPE_EVENT_DATA_BUFFER };
            r = xrPollEvent_(c->inst, &ev);
            if (r == XR_SUCCESS) {
                if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    const XrEventDataSessionStateChanged *e = (const XrEventDataSessionStateChanged *)&ev;
                    XLOG("session state -> %s", session_state_name(e->state));
                    if (e->state == XR_SESSION_STATE_READY) { ready = 1; break; }
                } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                    XLOG("instance loss pending");
                    break;
                }
                continue;
            }
            clock_gettime(CLOCK_MONOTONIC, &t);
            if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 >= o->wait_session_ms) break;
            struct timespec nap = { 0, 20 * 1000 * 1000 };
            nanosleep(&nap, NULL);
        }
        ledger("session reaches READY", ready, 0, ready ? "yes (frames are not submitted by the probe: X1)" : "not within %d ms", o->wait_session_ms);
    }
    return failed;
}

int bmvr_xr_probe(const struct bmvr_xr_probe_opts *opts)
{
    struct ctx c;
    memset(&c, 0, sizeof c);
    g_ledger_n = 0;
    int unavailable = 0;
    int failed = run_probe(&c, opts, &unavailable);
    ctx_close(&c);

    if (unavailable) {
        XLOG("xr-probe: UNAVAILABLE (no loader, no runtime, or no headset system; nothing was tested)");
        return BMVR_XR_PROBE_UNAVAILABLE;
    }
    int req_fail = failed;
    XLOG("---- summary ----");
    for (int i = 0; i < g_ledger_n; i++) {
        if (g_ledger[i].required && !g_ledger[i].ok) req_fail = 1;
        XLOG("%-4s %-40s %s", g_ledger[i].ok ? "ok" : (g_ledger[i].required ? "FAIL" : "note"), g_ledger[i].name, g_ledger[i].detail);
    }
    XLOG("xr-probe: %s", req_fail ? "FAILED (a required item above is missing)" : "PASS (nothing was submitted to the headset: frames are milestone X1)");
    return req_fail ? BMVR_XR_PROBE_FAILED : BMVR_XR_PROBE_OK;
}

/* ================================================================== X1: --selftest-xr
 *
 * A frame loop with a generated pattern and no game: the first time frames reach a headset through this code.
 *   - eye 0 (left):  blue field, big "L";  eye 1 (right): red field, big "R" (a wrong eye order is visible at once)
 *   - both: a 16-bit frame counter (white = 1, black = 0) at the top and a yellow marker that moves along the bottom
 *   - quad mode shows eye 0 on a head-locked quad 2 m in front; projection mode submits both eyes as a stereo
 *     projection layer, head-locked (VIEW space), with the runtime's own eye poses and FOVs.
 * It uses the design's raw-copy path on purpose: the pattern lives in R8G8B8A8_UNORM images (like the helper's shared images)
 * and is copied byte for byte (vkCmdCopyImage, no shader) into an R8G8B8A8_SRGB swapchain.
 *
 * One thread, one queue, no per-frame disk writes and no per-frame log lines: the only output while it runs is a rate line
 * every 5 s and the session state changes. What it can NOT show: that the picture is right (look at the headset), anything
 * about the game, the pair ring or poses (X2-X4).
 */
#define XR_ST_FUNCS(X) \
    X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrCreateReferenceSpace) \
    X(xrDestroySpace) X(xrLocateViews)

#define VK_ST_DEV_FUNCS(X) \
    X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkCmdCopyBufferToImage) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory)

#define DECL(name) static PFN_##name name##_;
XR_ST_FUNCS(DECL)
VK_ST_DEV_FUNCS(DECL)
#undef DECL
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties_;

#define ST_FRAMES 4 /* command buffers / fences in flight */
#define ST_BITS 16
#define ST_TILES 3 /* tile 0 = black, 1 = white, 2 = yellow marker */

static volatile sig_atomic_t g_st_stop;
static void st_on_signal(int sig) { (void)sig; g_st_stop = 1; }

struct st {
    struct ctx c;
    uint32_t qfam;
    VkQueue queue;
    uint32_t w, h;
    int usage_color_att; /* 1 if the swapchains were created with COLOR_ATTACHMENT usage too */
    XrEnvironmentBlendMode blend;
    XrViewConfigurationView views[2];
    XrSpace view_space;
    XrSwapchain sc[2];
    uint32_t nimg[2];
    XrSwapchainImageVulkanKHR *img[2];
    VkPhysicalDeviceMemoryProperties mp;
    VkCommandPool pool;
    VkCommandBuffer cb[ST_FRAMES];
    VkFence fence[ST_FRAMES];
    int fence_used[ST_FRAMES];
    VkImage pat[2];
    VkDeviceMemory pat_mem[2];
    VkBuffer tiles;
    VkDeviceMemory tiles_mem;
    uint32_t tile; /* tile edge in pixels */
    XrSessionState state;
    int running;
};

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static int find_mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static void st_close(struct st *s)
{
    struct ctx *c = &s->c;
    if (c->dev && vkDeviceWaitIdle_) vkDeviceWaitIdle_(c->dev); /* nothing of ours may still be in flight */
    if (c->dev) {
        for (int i = 0; i < ST_FRAMES; i++) if (s->fence[i] && vkDestroyFence_) vkDestroyFence_(c->dev, s->fence[i], NULL);
        if (s->pool && vkDestroyCommandPool_) vkDestroyCommandPool_(c->dev, s->pool, NULL); /* frees its command buffers */
        for (int e = 0; e < 2; e++) {
            if (s->pat[e] && vkDestroyImage_) vkDestroyImage_(c->dev, s->pat[e], NULL);
            if (s->pat_mem[e] && vkFreeMemory_) vkFreeMemory_(c->dev, s->pat_mem[e], NULL);
        }
        if (s->tiles && vkDestroyBuffer_) vkDestroyBuffer_(c->dev, s->tiles, NULL);
        if (s->tiles_mem && vkFreeMemory_) vkFreeMemory_(c->dev, s->tiles_mem, NULL);
    }
    for (int e = 0; e < 2; e++) {
        if (s->sc[e] && xrDestroySwapchain_) xrDestroySwapchain_(s->sc[e]);
        free(s->img[e]);
    }
    if (s->view_space && xrDestroySpace_) xrDestroySpace_(s->view_space);
    ctx_close(c); /* session, device, Vulkan instance, OpenXR instance (c->sc is unused here) */
}

static void xr_hint_no_runtime(void)
{
    XLOG("xrEnumerateInstanceExtensionProperties failed: no OpenXR runtime installed or active?");
    XLOG("check the active runtime (XR_RUNTIME_JSON, ~/.config/openxr/1/active_runtime.json, /etc/xdg/openxr/1/active_runtime.json, SteamVR/Monado settings)");
    XLOG("SteamVR: running is not enough, it must be the active runtime: ln -sf <SteamVR dir>/steamxr_linux64.json ~/.config/openxr/1/active_runtime.json");
}

/* Same sequence and the same device flags as the probe (X0), without its report. Returns 1 on a failure (ledgered). */
static int st_open(struct st *s, const struct bmvr_xr_selftest_opts *o, int *unavailable)
{
    struct ctx *c = &s->c;
    XrResult r;
    VkResult vr = VK_SUCCESS;

    if (!load_xr_loader(c)) { *unavailable = 1; return 0; }
    PFN_xrEnumerateInstanceExtensionProperties enumExt = NULL;
    PFN_xrCreateInstance createInst = NULL;
    xrGetInstanceProcAddr_(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enumExt);
    xrGetInstanceProcAddr_(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&createInst);
    if (!enumExt || !createInst) { XLOG("loader does not expose xrCreateInstance"); *unavailable = 1; return 0; }
    uint32_t next = 0;
    if (XR_FAILED(enumExt(NULL, 0, &next, NULL))) { xr_hint_no_runtime(); *unavailable = 1; return 0; }
    XrExtensionProperties *ext = calloc(next ? next : 1, sizeof *ext);
    for (uint32_t i = 0; i < next; i++) ext[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    if (XR_FAILED(enumExt(NULL, next, &next, ext))) next = 0;
    int has_enable2 = 0;
    for (uint32_t i = 0; i < next; i++) if (!strcmp(ext[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) has_enable2 = 1;
    free(ext);
    ledger("XR_KHR_vulkan_enable2", has_enable2, 1, has_enable2 ? "offered" : "NOT offered by this runtime");
    if (!has_enable2) return 1;

    const char *want_ext[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { .type = XR_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = 1, .enabledExtensionNames = want_ext };
    snprintf(ici.applicationInfo.applicationName, sizeof ici.applicationInfo.applicationName, "bmvr_xfer_helper");
    ici.applicationInfo.applicationVersion = 1;
    snprintf(ici.applicationInfo.engineName, sizeof ici.applicationInfo.engineName, "bmvr");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    r = createInst(&ici, &c->inst);
    if (r == XR_ERROR_RUNTIME_UNAVAILABLE || r == XR_ERROR_RUNTIME_FAILURE) {
        XLOG("xrCreateInstance failed (%d): the runtime is installed but not usable right now", (int)r);
        XLOG("Monado: start monado-service first. SteamVR: start SteamVR (and set it as the active OpenXR runtime).");
        *unavailable = 1;
        return 0;
    }
    if (XR_FAILED(r)) { ledger("xrCreateInstance", 0, 1, "%s", xr_rs(XR_NULL_HANDLE, r)); return 1; }
#define LOADX(name) xrGetInstanceProcAddr_(c->inst, #name, (PFN_xrVoidFunction *)&name##_); \
    if (!name##_) { ledger("load " #name, 0, 1, "missing in this runtime/loader"); return 1; }
    XR_INSTANCE_FUNCS(LOADX)
    XR_ST_FUNCS(LOADX)
#undef LOADX
    XrInstanceProperties ip = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties_(c->inst, &ip);
    ledger("xrCreateInstance", 1, 1, "runtime \"%s\" %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
           XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    r = xrGetSystem_(c->inst, &sgi, &c->sys);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        XLOG("the runtime is there but reports no head-mounted display (headset off / not connected, or a simulated driver is not enabled)");
        *unavailable = 1;
        return 0;
    }
    if (XR_FAILED(r)) { ledger("xrGetSystem", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }
    XrSystemProperties sp = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties_(c->inst, c->sys, &sp);
    ledger("xrGetSystem", 1, 1, "\"%s\"", sp.systemName);

    for (int i = 0; i < 2; i++) s->views[i] = (XrViewConfigurationView){ .type = XR_TYPE_VIEW_CONFIGURATION_VIEW };
    uint32_t nv = 0;
    r = xrEnumerateViewConfigurationViews_(c->inst, c->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, s->views);
    if (XR_FAILED(r) || nv != 2) { ledger("view sizes", 0, 1, "%s, %u views", xr_rs(c->inst, r), nv); return 1; }
    s->w = o->width > 0 ? (uint32_t)o->width : s->views[0].recommendedImageRectWidth;
    s->h = o->height > 0 ? (uint32_t)o->height : s->views[0].recommendedImageRectHeight;
    ledger("eye image size", 1, 1, "%ux%u (runtime recommends %ux%u)", s->w, s->h, s->views[0].recommendedImageRectWidth,
           s->views[0].recommendedImageRectHeight);

    XrEnvironmentBlendMode bm[4];
    uint32_t nbm = 0;
    xrEnumerateEnvironmentBlendModes_(c->inst, c->sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 4, &nbm, bm);
    s->blend = nbm ? bm[0] : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    for (uint32_t i = 0; i < nbm; i++) if (bm[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) s->blend = bm[i];

    XrGraphicsRequirementsVulkanKHR req = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    r = xrGetVulkanGraphicsRequirements2KHR_(c->inst, c->sys, &req);
    if (XR_FAILED(r)) { ledger("Vulkan requirements", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }
    c->vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!c->vk_lib) { ledger("libvulkan.so.1", 0, 1, "%s", dlerror()); return 1; }
    vkGetInstanceProcAddr_ = (PFN_vkGetInstanceProcAddr)dlsym(c->vk_lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr_) { ledger("libvulkan.so.1", 0, 1, "vkGetInstanceProcAddr missing"); return 1; }
    uint32_t vk_api = VK_API_VERSION_1_1;
    if (xr_ver_to_vk_minor_key(req.minApiVersionSupported) > vk_ver_minor_key(vk_api))
        vk_api = VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported), 0);
    if (xr_ver_to_vk_minor_key(req.maxApiVersionSupported) < vk_ver_minor_key(vk_api)) {
        ledger("Vulkan version window", 0, 1, "runtime maximum is below Vulkan 1.1");
        return 1;
    }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "bmvr_xfer_helper", .apiVersion = vk_api };
    VkInstanceCreateInfo vici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    XrVulkanInstanceCreateInfoKHR xvici = { .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = c->sys,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr_, .vulkanCreateInfo = &vici };
    r = xrCreateVulkanInstanceKHR_(c->inst, &xvici, &c->vk_inst, &vr);
    if (XR_FAILED(r) || vr != VK_SUCCESS) { ledger("xrCreateVulkanInstanceKHR", 0, 1, "%s, VkResult %d", xr_rs(c->inst, r), (int)vr); return 1; }
#define LOADV(name) name##_ = (PFN_##name)vkGetInstanceProcAddr_(c->vk_inst, #name); \
    if (!name##_) { ledger("load " #name, 0, 1, "missing"); return 1; }
    VK_INST_FUNCS(LOADV)
    LOADV(vkGetPhysicalDeviceMemoryProperties)
#undef LOADV
    XrVulkanGraphicsDeviceGetInfoKHR gdi = { .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = c->sys, .vulkanInstance = c->vk_inst };
    r = xrGetVulkanGraphicsDevice2KHR_(c->inst, &gdi, &c->phys);
    if (XR_FAILED(r)) { ledger("xrGetVulkanGraphicsDevice2KHR", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }
    VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    vkGetPhysicalDeviceProperties2_(c->phys, &pp);
    ledger("runtime-chosen GPU", 1, 1, "\"%s\"", pp.properties.deviceName);
    vkGetPhysicalDeviceMemoryProperties_(c->phys, &s->mp);

    uint32_t nde = 0;
    vkEnumerateDeviceExtensionProperties_(c->phys, NULL, &nde, NULL);
    VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof *de);
    vkEnumerateDeviceExtensionProperties_(c->phys, NULL, &nde, de);
    int has_memfd = 0, has_semfd = 0;
    for (uint32_t i = 0; i < nde; i++) {
        if (!strcmp(de[i].extensionName, "VK_KHR_external_memory_fd")) has_memfd = 1;
        if (!strcmp(de[i].extensionName, "VK_KHR_external_semaphore_fd")) has_semfd = 1;
    }
    free(de);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties_(c->phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties_(c->phys, &nq, qf);
    int qfam = -1;
    for (uint32_t i = 0; i < nq; i++) if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qfam < 0) qfam = (int)i;
    free(qf);
    if (qfam < 0) { ledger("graphics queue family", 0, 1, "none"); return 1; }
    s->qfam = (uint32_t)qfam;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = s->qfam, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dext[2];
    uint32_t ndext = 0;
    if (has_memfd) dext[ndext++] = "VK_KHR_external_memory_fd";
    if (has_semfd) dext[ndext++] = "VK_KHR_external_semaphore_fd";
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = ndext, .ppEnabledExtensionNames = dext };
    XrVulkanDeviceCreateInfoKHR xvdi = { .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = c->sys, .pfnGetInstanceProcAddr = vkGetInstanceProcAddr_,
        .vulkanPhysicalDevice = c->phys, .vulkanCreateInfo = &dci };
    r = xrCreateVulkanDeviceKHR_(c->inst, &xvdi, &c->dev, &vr);
    if (XR_FAILED(r) || vr != VK_SUCCESS) { ledger("xrCreateVulkanDeviceKHR", 0, 1, "%s, VkResult %d", xr_rs(c->inst, r), (int)vr); return 1; }
    vkDestroyDevice_ = (PFN_vkDestroyDevice)vkGetDeviceProcAddr_(c->dev, "vkDestroyDevice");
#define LOADD(name) name##_ = (PFN_##name)vkGetDeviceProcAddr_(c->dev, #name); \
    if (!name##_) { ledger("load " #name, 0, 1, "missing"); return 1; }
    VK_ST_DEV_FUNCS(LOADD)
#undef LOADD
    vkGetDeviceQueue_(c->dev, s->qfam, 0, &s->queue);
    ledger("Vulkan device", 1, 1, "created on the runtime-chosen GPU (external memory/semaphore fd: %d/%d)", has_memfd, has_semfd);

    XrGraphicsBindingVulkanKHR gb = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = c->vk_inst, .physicalDevice = c->phys,
        .device = c->dev, .queueFamilyIndex = s->qfam, .queueIndex = 0 };
    XrSessionCreateInfo sci = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &gb, .systemId = c->sys };
    r = xrCreateSession_(c->inst, &sci, &c->sess);
    if (XR_FAILED(r)) { ledger("xrCreateSession", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }
    ledger("xrCreateSession", 1, 1, "created");
    return 0;
}

/* Swapchains (one per eye), their images, the reference space. */
static int st_make_swapchains(struct st *s)
{
    struct ctx *c = &s->c;
    XrResult r;
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats_(c->sess, 0, &nf, NULL);
    int64_t *fmts = calloc(nf ? nf : 1, sizeof *fmts);
    xrEnumerateSwapchainFormats_(c->sess, nf, &nf, fmts);
    int has_srgb = 0;
    for (uint32_t i = 0; i < nf; i++) if (fmts[i] == 43) has_srgb = 1;
    free(fmts);
    ledger("swapchain format R8G8B8A8_SRGB", has_srgb, 1, has_srgb ? "offered: the raw-copy design works" : "NOT offered: needs the shader fallback (design doc, Decision 3); refusing to guess");
    if (!has_srgb) return 1;

    for (int pass = 0; pass < 2; pass++) { /* with COLOR_ATTACHMENT usage first (what a layout hand-over to the runtime implies), then the X0-verified flags */
        XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                                      (pass == 0 ? XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT : 0);
        int made = 0;
        for (int e = 0; e < 2; e++) {
            XrSwapchainCreateInfo sci = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .usageFlags = usage, .format = 43, .sampleCount = 1,
                .width = s->w, .height = s->h, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
            r = xrCreateSwapchain_(c->sess, &sci, &s->sc[e]);
            if (XR_FAILED(r)) {
                if (pass == 1) { ledger("xrCreateSwapchain", 0, 1, "eye %d, %ux%u R8G8B8A8_SRGB: %s", e, s->w, s->h, xr_rs(c->inst, r)); return 1; }
                break;
            }
            made++;
        }
        if (made == 2) { s->usage_color_att = (pass == 0); break; }
        for (int e = 0; e < 2; e++) if (s->sc[e]) { xrDestroySwapchain_(s->sc[e]); s->sc[e] = 0; }
    }
    for (int e = 0; e < 2; e++) {
        uint32_t n = 0;
        xrEnumerateSwapchainImages_(s->sc[e], 0, &n, NULL);
        s->img[e] = calloc(n ? n : 1, sizeof *s->img[e]);
        for (uint32_t i = 0; i < n; i++) s->img[e][i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        r = xrEnumerateSwapchainImages_(s->sc[e], n, &n, (XrSwapchainImageBaseHeader *)s->img[e]);
        if (XR_FAILED(r) || !n) { ledger("xrEnumerateSwapchainImages", 0, 1, "eye %d: %s", e, xr_rs(c->inst, r)); return 1; }
        s->nimg[e] = n;
    }
    ledger("xrCreateSwapchain", 1, 1, "2 x %ux%u R8G8B8A8_SRGB, %u image(s) each, usage TRANSFER_DST|SAMPLED%s", s->w, s->h, s->nimg[0],
           s->usage_color_att ? "|COLOR_ATTACHMENT" : " (runtime refused COLOR_ATTACHMENT)");

    XrReferenceSpaceCreateInfo rsi = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW,
        .poseInReferenceSpace = { .orientation = { 0, 0, 0, 1 } } };
    r = xrCreateReferenceSpace_(c->sess, &rsi, &s->view_space);
    if (XR_FAILED(r)) { ledger("xrCreateReferenceSpace(VIEW)", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }
    return 0;
}

/* 5x7 glyphs for the eye letters ('L', 'R'); one bit per column, MSB = leftmost of 5. */
static const uint8_t glyph_L[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f };
static const uint8_t glyph_R[7] = { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 };

static void st_fill_pattern(uint8_t *px, uint32_t w, uint32_t h, int eye)
{
    const uint8_t bg[3] = { eye ? 130 : 20, eye ? 30 : 50, eye ? 30 : 140 };
    const uint32_t border = h / 120 + 2;
    const uint32_t cell = h / 14;
    const uint32_t gx0 = (w - 5 * cell) / 2, gy0 = (h - 7 * cell) / 2;
    const uint8_t *glyph = eye ? glyph_R : glyph_L;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t col[3] = { bg[0], bg[1], bg[2] };
            int grid = (x % (w / 8) < 2) || (y % (h / 8) < 2);
            if (grid) for (int k = 0; k < 3; k++) col[k] = (uint8_t)(col[k] + 40);
            if (x < border || y < border || x >= w - border || y >= h - border) col[0] = col[1] = col[2] = 255;
            if (x >= gx0 && y >= gy0 && x < gx0 + 5 * cell && y < gy0 + 7 * cell) {
                uint32_t gc = (x - gx0) / cell, gr = (y - gy0) / cell;
                if (glyph[gr] & (0x10u >> gc)) col[0] = col[1] = col[2] = 255;
            }
            uint8_t *d = px + ((size_t)y * w + x) * 4;
            d[0] = col[0]; d[1] = col[1]; d[2] = col[2]; d[3] = 255;
        }
    }
}

static int st_buffer(struct st *s, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem, void **map)
{
    VkDevice d = s->c.dev;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer_(d, &bi, NULL, buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements_(d, *buf, &mr);
    int mt = find_mem_type(&s->mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (vkAllocateMemory_(d, &ai, NULL, mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory_(d, *buf, *mem, 0) != VK_SUCCESS) return 0;
    return vkMapMemory_(d, *mem, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS;
}

static VkImageMemoryBarrier st_img_barrier(VkImage im, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    return b;
}

/* Pattern images (device local, left in TRANSFER_SRC_OPTIMAL), the tile buffer, command pool, command buffers, fences. */
static int st_make_gpu_objects(struct st *s)
{
    VkDevice d = s->c.dev;
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = s->qfam };
    if (vkCreateCommandPool_(d, &pci, NULL, &s->pool) != VK_SUCCESS) { ledger("command pool", 0, 1, "vkCreateCommandPool failed"); return 1; }
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = ST_FRAMES };
    if (vkAllocateCommandBuffers_(d, &cai, s->cb) != VK_SUCCESS) { ledger("command buffers", 0, 1, "vkAllocateCommandBuffers failed"); return 1; }
    for (int i = 0; i < ST_FRAMES; i++) {
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence_(d, &fi, NULL, &s->fence[i]) != VK_SUCCESS) { ledger("fences", 0, 1, "vkCreateFence failed"); return 1; }
    }

    /* tiles: black, white, yellow */
    s->tile = s->h / 48 < 8 ? 8 : s->h / 48;
    size_t tpx = (size_t)s->tile * s->tile;
    void *tmap = NULL;
    if (!st_buffer(s, (VkDeviceSize)(tpx * 4 * ST_TILES), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->tiles, &s->tiles_mem, &tmap)) {
        ledger("tile buffer", 0, 1, "could not create a host-visible buffer"); return 1;
    }
    {
        static const uint8_t col[ST_TILES][4] = { { 0, 0, 0, 255 }, { 255, 255, 255, 255 }, { 255, 220, 0, 255 } };
        uint8_t *t = tmap;
        for (int k = 0; k < ST_TILES; k++) for (size_t i = 0; i < tpx; i++) memcpy(t + (k * tpx + i) * 4, col[k], 4);
    }

    /* pattern images: filled on the CPU once, uploaded once; per frame everything stays on the GPU */
    VkBuffer stg = 0;
    VkDeviceMemory stg_mem = 0;
    void *smap = NULL;
    size_t bytes = (size_t)s->w * s->h * 4;
    if (!st_buffer(s, (VkDeviceSize)bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg, &stg_mem, &smap)) {
        ledger("staging buffer", 0, 1, "could not create a %zu MB host-visible buffer", bytes >> 20);
        if (stg) vkDestroyBuffer_(d, stg, NULL);
        if (stg_mem) vkFreeMemory_(d, stg_mem, NULL);
        return 1;
    }
    int fail = 0;
    for (int e = 0; e < 2 && !fail; e++) {
        VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { s->w, s->h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkMemoryRequirements mr;
        if (vkCreateImage_(d, &ii, NULL, &s->pat[e]) != VK_SUCCESS) { fail = 1; break; }
        vkGetImageMemoryRequirements_(d, s->pat[e], &mr);
        int mt = find_mem_type(&s->mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) mt = find_mem_type(&s->mp, mr.memoryTypeBits, 0);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)(mt < 0 ? 0 : mt) };
        if (mt < 0 || vkAllocateMemory_(d, &ai, NULL, &s->pat_mem[e]) != VK_SUCCESS || vkBindImageMemory_(d, s->pat[e], s->pat_mem[e], 0) != VK_SUCCESS) { fail = 1; break; }

        st_fill_pattern(smap, s->w, s->h, e);
        VkCommandBuffer cb = s->cb[0];
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkResetCommandBuffer_(cb, 0);
        vkBeginCommandBuffer_(cb, &bi);
        VkImageMemoryBarrier b1 = st_img_barrier(s->pat[e], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
        VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { s->w, s->h, 1 } };
        vkCmdCopyBufferToImage_(cb, stg, s->pat[e], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        VkImageMemoryBarrier b2 = st_img_barrier(s->pat[e], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
        vkEndCommandBuffer_(cb);
        vkResetFences_(d, 1, &s->fence[0]);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        if (vkQueueSubmit_(s->queue, 1, &si, s->fence[0]) != VK_SUCCESS || vkWaitForFences_(d, 1, &s->fence[0], VK_TRUE, 5000000000ull) != VK_SUCCESS) fail = 1;
    }
    vkDestroyBuffer_(d, stg, NULL); /* host-visible staging memory is released again: it was only needed for the upload */
    vkFreeMemory_(d, stg_mem, NULL);
    if (fail) { ledger("pattern images", 0, 1, "creating or uploading the pattern failed"); return 1; }
    ledger("pattern images", 1, 1, "2 x %ux%u R8G8B8A8_UNORM uploaded once; tile %u px, %d-bit counter", s->w, s->h, s->tile, ST_BITS);
    return 0;
}

/* Acquire, draw (GPU copies only) and release the images for one frame. eyes = 1 (quad) or 2 (projection). */
static int st_render(struct st *s, uint64_t frame_no, int eyes, const char **why)
{
    VkDevice d = s->c.dev;
    uint32_t idx[2] = { 0, 0 };
    XrResult r;
    for (int e = 0; e < eyes; e++) {
        r = xrAcquireSwapchainImage_(s->sc[e], NULL, &idx[e]);
        if (XR_FAILED(r)) { *why = "xrAcquireSwapchainImage"; return 1; }
        XrSwapchainImageWaitInfo wi = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 1000000000 /* 1 s */ };
        r = xrWaitSwapchainImage_(s->sc[e], &wi);
        if (XR_FAILED(r) || r == XR_TIMEOUT_EXPIRED) { *why = "xrWaitSwapchainImage"; return 1; }
    }
    int slot = (int)(frame_no % ST_FRAMES);
    if (s->fence_used[slot]) {
        if (vkWaitForFences_(d, 1, &s->fence[slot], VK_TRUE, 1000000000ull) != VK_SUCCESS) { *why = "vkWaitForFences (previous use of this command buffer)"; return 1; }
    }
    vkResetFences_(d, 1, &s->fence[slot]);
    VkCommandBuffer cb = s->cb[slot];
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer_(cb, 0);
    vkBeginCommandBuffer_(cb, &bi);

    VkBufferImageCopy regions[ST_BITS + 1];
    uint32_t t = s->tile, gap = t / 4, x0 = (s->w - (ST_BITS * t + (ST_BITS - 1) * gap)) / 2, y0 = s->h / 12;
    for (int i = 0; i < ST_BITS; i++) { /* bit 15 leftmost */
        int bit = (int)((frame_no >> (ST_BITS - 1 - i)) & 1);
        regions[i] = (VkBufferImageCopy){ .bufferOffset = (VkDeviceSize)(bit * (uint64_t)t * t * 4), .bufferRowLength = t, .bufferImageHeight = t,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageOffset = { (int32_t)(x0 + (uint32_t)i * (t + gap)), (int32_t)y0, 0 }, .imageExtent = { t, t, 1 } };
    }
    regions[ST_BITS] = (VkBufferImageCopy){ .bufferOffset = (VkDeviceSize)(2ull * t * t * 4), .bufferRowLength = t, .bufferImageHeight = t,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { (int32_t)((frame_no * 6) % (s->w - t)), (int32_t)(s->h - 3 * t), 0 }, .imageExtent = { t, t, 1 } };

    for (int e = 0; e < eyes; e++) {
        VkImage dst = s->img[e][idx[e]].image;
        /* oldLayout UNDEFINED is always legal and every texel is overwritten; the hand-back layout below is the spec's rule (verify on SteamVR) */
        VkImageMemoryBarrier a = st_img_barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &a);
        VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { s->w, s->h, 1 } };
        vkCmdCopyImage_(cb, s->pat[e], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkCmdCopyBufferToImage_(cb, s->tiles, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ST_BITS + 1, regions);
        VkImageMemoryBarrier z = st_img_barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &z);
    }
    vkEndCommandBuffer_(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    if (vkQueueSubmit_(s->queue, 1, &si, s->fence[slot]) != VK_SUCCESS) { *why = "vkQueueSubmit"; return 1; }
    s->fence_used[slot] = 1;
    for (int e = 0; e < eyes; e++) {
        r = xrReleaseSwapchainImage_(s->sc[e], NULL);
        if (XR_FAILED(r)) { *why = "xrReleaseSwapchainImage"; return 1; }
    }
    return 0;
}

struct st_stats {
    double win_start, last_ts, dt_min, dt_max, dt_sum, wait_sum, cpu_sum;
    uint64_t frames, rendered, late, win_frames, win_rendered, win_late;
    double period_ms;
    double win_dt_min, win_dt_max, win_dt_sum, win_wait_sum, win_cpu_sum;
};

static void st_win_reset(struct st_stats *t, double now)
{
    t->win_start = now;
    t->win_frames = t->win_rendered = t->win_late = 0;
    t->win_dt_min = 1e9; t->win_dt_max = 0; t->win_dt_sum = 0; t->win_wait_sum = 0; t->win_cpu_sum = 0;
}

static void st_win_print(const char *mode, const struct st_stats *t, double now)
{
    double secs = (now - t->win_start) / 1000.0;
    if (t->win_frames < 2 || secs <= 0) return;
    XLOG("selftest[%s]: %.1f fps (display period %.2f ms), frame interval min/avg/max %.2f/%.2f/%.2f ms, rendered %llu/%llu, late %llu, wait avg %.2f ms, cpu avg %.2f ms",
         mode, (double)t->win_frames / secs, t->period_ms, t->win_dt_min, t->win_dt_sum / (double)(t->win_frames - 1), t->win_dt_max,
         (unsigned long long)t->win_rendered, (unsigned long long)t->win_frames, (unsigned long long)t->win_late,
         t->win_wait_sum / (double)t->win_frames, t->win_cpu_sum / (double)t->win_frames);
}

/* Drain the event queue. Returns 1 if the session must end (STOPPING handled, EXITING, LOSS_PENDING), 0 otherwise. */
static int st_events(struct st *s, int *saw_ready)
{
    struct ctx *c = &s->c;
    int end = 0;
    for (;;) {
        XrEventDataBuffer ev = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        if (xrPollEvent_(c->inst, &ev) != XR_SUCCESS) break;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *e = (const XrEventDataSessionStateChanged *)&ev;
            s->state = e->state;
            XLOG("session state -> %s", session_state_name(e->state));
            if (e->state == XR_SESSION_STATE_READY) {
                if (saw_ready) *saw_ready = 1;
                if (!s->running) {
                    XrSessionBeginInfo bi = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
                    XrResult r = xrBeginSession_(c->sess, &bi);
                    if (XR_FAILED(r)) { ledger("xrBeginSession", 0, 1, "%s", xr_rs(c->inst, r)); end = 1; }
                    else { s->running = 1; ledger("xrBeginSession", 1, 1, "PRIMARY_STEREO"); }
                }
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                if (s->running) { xrEndSession_(c->sess); s->running = 0; }
                end = 1;
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                end = 1;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            XLOG("instance loss pending");
            end = 1;
        }
    }
    return end;
}

/* One layer mode (0 = quad, 1 = projection) for `seconds` seconds. Returns 1 on a failure that ends the test. */
static int st_run_mode(struct st *s, int projection, int seconds, uint64_t *frame_no, int *session_ended)
{
    struct ctx *c = &s->c;
    const char *mode = projection ? "projection" : "quad";
    struct st_stats t;
    memset(&t, 0, sizeof t);
    t.dt_min = 1e9;
    double t_phase = now_ms();
    st_win_reset(&t, t_phase);
    int view_fallback_logged = 0;
    XLOG("selftest[%s]: running for %d s (left eye: BLUE with 'L', right eye: RED with 'R'; top: 16-bit frame counter, bottom: moving marker)", mode, seconds);

    while (!g_st_stop && now_ms() - t_phase < seconds * 1000.0) {
        if (st_events(s, NULL)) { *session_ended = 1; break; }
        if (!s->running) { struct timespec nap = { 0, 5 * 1000 * 1000 }; nanosleep(&nap, NULL); continue; }

        XrFrameState fs = { .type = XR_TYPE_FRAME_STATE };
        double tw0 = now_ms();
        XrResult r = xrWaitFrame_(c->sess, NULL, &fs);
        double tw1 = now_ms();
        if (XR_FAILED(r)) { ledger("xrWaitFrame", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }
        r = xrBeginFrame_(c->sess, NULL);
        if (XR_FAILED(r)) { ledger("xrBeginFrame", 0, 1, "%s", xr_rs(c->inst, r)); return 1; }

        t.period_ms = (double)fs.predictedDisplayPeriod / 1e6;
        t.frames++; t.win_frames++;
        if (t.last_ts > 0) {
            double dt = tw1 - t.last_ts;
            if (dt < t.win_dt_min) t.win_dt_min = dt;
            if (dt > t.win_dt_max) t.win_dt_max = dt;
            t.win_dt_sum += dt;
            if (dt < t.dt_min) t.dt_min = dt;
            if (dt > t.dt_max) t.dt_max = dt;
            t.dt_sum += dt;
            if (t.period_ms > 0 && dt > 1.5 * t.period_ms) { t.late++; t.win_late++; }
        }
        t.last_ts = tw1;
        t.win_wait_sum += tw1 - tw0;
        t.wait_sum += tw1 - tw0;

        XrCompositionLayerQuad quad;
        XrCompositionLayerProjection proj;
        XrCompositionLayerProjectionView pv[2];
        const XrCompositionLayerBaseHeader *layers[1];
        uint32_t nlayers = 0;
        if (fs.shouldRender) {
            double tc0 = now_ms();
            const char *why = "";
            if (st_render(s, *frame_no, projection ? 2 : 1, &why)) { ledger("render", 0, 1, "%s failed (frame %llu)", why, (unsigned long long)*frame_no); return 1; }
            XrRect2Di rect = { { 0, 0 }, { (int32_t)s->w, (int32_t)s->h } };
            if (!projection) {
                quad = (XrCompositionLayerQuad){ .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .layerFlags = 0, .space = s->view_space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                    .subImage = { .swapchain = s->sc[0], .imageRect = rect, .imageArrayIndex = 0 },
                    .pose = { .orientation = { 0, 0, 0, 1 }, .position = { 0, 0, -2.0f } }, .size = { 1.6f * (float)s->w / (float)s->h, 1.6f } };
                layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&quad;
            } else {
                XrView views[2] = { { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW } };
                XrViewState vs = { .type = XR_TYPE_VIEW_STATE };
                XrViewLocateInfo li = { .type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                    .displayTime = fs.predictedDisplayTime, .space = s->view_space };
                uint32_t nvw = 0;
                r = xrLocateViews_(c->sess, &li, &vs, 2, &nvw, views);
                int ok = XR_SUCCEEDED(r) && nvw == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
                if (!ok) { /* pinned fallback: identity orientation, eyes 64 mm apart, symmetric 90 degree FOV */
                    if (!view_fallback_logged) { XLOG("selftest[projection]: xrLocateViews gave no valid views (%s, flags 0x%llx): using pinned eye poses", xr_rs(c->inst, r), (unsigned long long)vs.viewStateFlags); view_fallback_logged = 1; }
                    for (int e = 0; e < 2; e++) {
                        views[e].pose = (XrPosef){ .orientation = { 0, 0, 0, 1 }, .position = { e ? 0.032f : -0.032f, 0, 0 } };
                        views[e].fov = (XrFovf){ -0.785398f, 0.785398f, 0.785398f, -0.785398f };
                    }
                }
                for (int e = 0; e < 2; e++) {
                    pv[e] = (XrCompositionLayerProjectionView){ .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = views[e].pose, .fov = views[e].fov,
                        .subImage = { .swapchain = s->sc[e], .imageRect = rect, .imageArrayIndex = 0 } };
                }
                proj = (XrCompositionLayerProjection){ .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .layerFlags = 0, .space = s->view_space, .viewCount = 2, .views = pv };
                layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&proj;
            }
            double tc1 = now_ms();
            t.win_cpu_sum += tc1 - tc0; t.cpu_sum += tc1 - tc0;
            t.rendered++; t.win_rendered++;
            (*frame_no)++;
        }
        XrFrameEndInfo ei = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = fs.predictedDisplayTime, .environmentBlendMode = s->blend, .layerCount = nlayers, .layers = nlayers ? layers : NULL };
        r = xrEndFrame_(c->sess, &ei);
        if (XR_FAILED(r)) { ledger("xrEndFrame", 0, 1, "%s (frame %llu)", xr_rs(c->inst, r), (unsigned long long)*frame_no); return 1; }

        double now = now_ms();
        if (now - t.win_start >= 5000.0) { st_win_print(mode, &t, now); st_win_reset(&t, now); }
    }
    double now = now_ms();
    st_win_print(mode, &t, now);

    double avg = t.frames > 1 ? t.dt_sum / (double)(t.frames - 1) : 0;
    ledger("frames submitted", t.rendered > 0, 1, "%s: %llu rendered of %llu frames%s", mode, (unsigned long long)t.rendered, (unsigned long long)t.frames,
           t.rendered ? "" : " (shouldRender was never true: headset not worn / session not visible?)");
    int steady = t.frames > 10 && t.period_ms > 0 && avg < t.period_ms * 1.15 && avg > t.period_ms * 0.85 && t.dt_max < t.period_ms * 2.5;
    ledger("cadence", steady, 0, "%s: avg %.2f ms vs period %.2f ms, max %.2f ms, late(>1.5x) %llu of %llu", mode, avg, t.period_ms, t.dt_max,
           (unsigned long long)t.late, (unsigned long long)t.frames);
    return 0;
}

int bmvr_xr_selftest(const struct bmvr_xr_selftest_opts *o)
{
    struct st *s = calloc(1, sizeof *s);
    if (!s) return BMVR_XR_PROBE_FAILED;
    g_ledger_n = 0;
    g_st_stop = 0;
    int unavailable = 0, failed = 0;
    struct sigaction sa = { .sa_handler = st_on_signal }, old_int, old_term;
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);

    failed = st_open(s, o, &unavailable);
    if (!failed && !unavailable) failed = st_make_swapchains(s);
    if (!failed && !unavailable) failed = st_make_gpu_objects(s);
    if (!failed && !unavailable) {
        /* wait for READY (st_events begins the session), as the probe does */
        int ready = 0;
        double t0 = now_ms();
        int ended = 0;
        while (!g_st_stop && !s->running && !ended && now_ms() - t0 < (o->wait_ready_ms > 0 ? o->wait_ready_ms : 10000)) {
            ended = st_events(s, &ready);
            if (!s->running) { struct timespec nap = { 0, 20 * 1000 * 1000 }; nanosleep(&nap, NULL); }
        }
        ledger("session reaches READY", ready, 1, ready ? "yes" : "not within %d ms (is SteamVR showing the headset as active?)", o->wait_ready_ms);
        if (!s->running) failed = 1;
    }
    if (!failed && !unavailable) {
        int ended = 0;
        uint64_t frame_no = 0;
        int seconds = o->seconds > 0 ? o->seconds : 10;
        if (o->layer_mask & 1) failed = st_run_mode(s, 0, seconds, &frame_no, &ended);
        if (!failed && !ended && !g_st_stop && (o->layer_mask & 2)) failed = st_run_mode(s, 1, seconds, &frame_no, &ended);
        if (ended) ledger("session kept running", 0, 1, "the runtime ended the session before the test was over (state %s)", session_state_name(s->state));
        if (g_st_stop) XLOG("interrupted: shutting down cleanly");
        /* leave properly: request exit, keep the frame loop alive (empty layers) until the runtime says STOPPING */
        if (s->running && !failed) {
            xrRequestExitSession_(s->c.sess);
            double t0 = now_ms();
            while (s->running && now_ms() - t0 < 3000) {
                XrFrameState fs = { .type = XR_TYPE_FRAME_STATE };
                if (XR_SUCCEEDED(xrWaitFrame_(s->c.sess, NULL, &fs)) && XR_SUCCEEDED(xrBeginFrame_(s->c.sess, NULL))) {
                    XrFrameEndInfo ei = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = fs.predictedDisplayTime, .environmentBlendMode = s->blend };
                    xrEndFrame_(s->c.sess, &ei);
                }
                st_events(s, NULL);
            }
            ledger("session ends cleanly", !s->running, 0, s->running ? "no STOPPING within 3 s" : "xrRequestExitSession -> STOPPING -> xrEndSession");
        }
    }
    st_close(s);
    free(s);
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);

    if (unavailable) {
        XLOG("xr-selftest: UNAVAILABLE (no loader, no runtime, or no headset system; nothing was tested)");
        return BMVR_XR_PROBE_UNAVAILABLE;
    }
    int req_fail = failed;
    XLOG("---- summary ----");
    for (int i = 0; i < g_ledger_n; i++) {
        if (g_ledger[i].required && !g_ledger[i].ok) req_fail = 1;
        XLOG("%-4s %-34s %s", g_ledger[i].ok ? "ok" : (g_ledger[i].required ? "FAIL" : "note"), g_ledger[i].name, g_ledger[i].detail);
    }
    XLOG("eye order, colours, the counter and the marker are NOT checked by this program: look in the headset (left BLUE 'L', right RED 'R')");
    XLOG("xr-selftest: %s", req_fail ? "FAILED (a required item above is missing)" : "PASS (frames were submitted; whether the picture is right is a visual check)");
    return req_fail ? BMVR_XR_PROBE_FAILED : BMVR_XR_PROBE_OK;
}
