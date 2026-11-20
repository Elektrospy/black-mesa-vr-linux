/*
 * Black Mesa VR - native Linux port, stage 4a: OpenXR sink (milestone X0: probe). See bmvr_xr_sink.h and
 * docs/LINUX_OPENXR_CONSUMER.md.
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
