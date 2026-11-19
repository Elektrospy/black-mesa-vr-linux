/*
 * Black Mesa VR - native Linux port, stages 2a/2b/3a: 64-bit helper for the GPU hand-off test.
 *
 * What it does:
 *   1. Creates a Vulkan image whose memory can be exported (VK_KHR_external_memory_fd).
 *   2. Listens on "<game folder>/bmvr_xfer.sock". When the 32-bit shim inside the game connects,
 *      it sends the exported memory fd; the shim imports it as an OpenGL texture
 *      (GL_EXT_memory_object_fd) and blits the game's frame into it every ~2 s.
 *   3. After each frame message it copies the image into host memory and writes
 *      bmvr_snapshot_<n>_<what>_flip<0|1>.ppm into the game folder (first few only), then acks.
 *      <what> is the engine render-target label the shim copied (or "window"); flip says whether
 *      the shim inverted Y. With the default flip=alt the snapshots alternate, so the pair shows
 *      which orientation is upright.
 *
 * If a snapshot shows the game picture, GPU memory sharing between the 32-bit game and a
 * 64-bit process works on this system: the prerequisite for the OpenXR path.
 *
 * Vulkan is loaded with dlopen (no libvulkan dev package needed); only the Vulkan HEADERS are
 * needed to build (vulkan-headers; scripts/build-linux.sh falls back to the copy in research/).
 *
 * Usage:  bmvr_xfer_helper [game folder] [--size WxH] [--gpu N] [--snapshots N] [--eyes 1|2] [--no-semaphores]
 *         --eyes 2 (stage 3d) offers two shared images; the game uses the second only with target2 in bmvr_target.txt
        bmvr_xfer_helper --selftest-sync [--gpu N]      (no game needed: checks semaphore sync on this GPU)
 *         (game folder defaults to the folder this program lives in)
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "bmvr_xfer.h"
#include "bmvr_single_instance.h"

/* ------------------------------------------------------------------ helpers */
#define LOG(...) do { fprintf(stderr, "[helper] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define DIE(...) do { fprintf(stderr, "[helper] ERROR: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define VKCHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) DIE("%s failed: VkResult %d", #call, (int)r_); } while (0)

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* ------------------------------------------------------------ Vulkan loader */
static PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
static PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr;

#define VK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceImageFormatProperties2) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties)

#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements2) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) \
    X(vkGetMemoryFdKHR) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) \
    X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkDeviceWaitIdle) X(vkCreateSemaphore) X(vkDestroySemaphore)

#define DECL(name) static PFN_##name name;
VK_INSTANCE_FUNCS(DECL)
VK_DEVICE_FUNCS(DECL)
#undef DECL

/* ------------------------------------------------------------------- state */
static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;
#define MAX_EYES 2                         /* stage 3d: shared images per client (--eyes 1|2); eye 0 = the image of the earlier stages */
static int g_eyes = 1;
static VkImage g_image[MAX_EYES];
static VkDeviceMemory g_image_mem[MAX_EYES];
static VkDeviceSize g_image_mem_size;      /* identical for every image (same size and format) */
static VkBuffer g_buf[MAX_EYES];
static VkDeviceMemory g_buf_mem[MAX_EYES];
static void *g_buf_map[MAX_EYES];
static VkCommandPool g_pool;
static VkCommandBuffer g_cmd;
static VkFence g_fence;
/* Stage 3a: GPU semaphores shared with the game's GL context (VK_KHR_external_semaphore_fd). */
static PFN_vkGetSemaphoreFdKHR pfnGetSemFd;
static PFN_vkImportSemaphoreFdKHR pfnImportSemFd;
static int g_sem_ok;                       /* this GPU/driver can export + import opaque-fd semaphores */
static int g_no_sem;                       /* --no-semaphores */
static VkSemaphore g_sem_gl_ready;         /* GL signals, Vulkan waits   (frame is in the image) */
static VkSemaphore g_sem_vk_done;          /* Vulkan signals, GL waits   (helper finished reading it) */
static int g_sem_active;                   /* semaphores exist for the current client */
static uint32_t g_w = 1920, g_h = 1080;

static int find_memory_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static void print_uuid(const char *label, const uint8_t u[VK_UUID_SIZE])
{
    char s[64];
    int n = 0;
    for (unsigned i = 0; i < VK_UUID_SIZE; i++) n += snprintf(s + n, sizeof s - (size_t)n, "%02x", u[i]);
    LOG("%s%s", label, s);
}

static void vulkan_init(int want_gpu)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) DIE("cannot load libvulkan.so.1 (%s). Install your GPU's Vulkan driver / vulkan-icd-loader.", dlerror());
    pfnGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!pfnGetInstanceProcAddr) DIE("vkGetInstanceProcAddr missing");
    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)pfnGetInstanceProcAddr(NULL, "vkCreateInstance");

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "bmvr_xfer_helper",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VKCHECK(createInstance(&ici, NULL, &g_inst));
#define LOADI(name) name = (PFN_##name)pfnGetInstanceProcAddr(g_inst, #name); if (!name) DIE("missing " #name);
    VK_INSTANCE_FUNCS(LOADI)
#undef LOADI
    pfnGetDeviceProcAddr = vkGetDeviceProcAddr;

    uint32_t n = 0;
    VKCHECK(vkEnumeratePhysicalDevices(g_inst, &n, NULL));
    if (!n) DIE("no Vulkan devices found");
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    VKCHECK(vkEnumeratePhysicalDevices(g_inst, &n, devs));

    int pick = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceIDProperties idp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &idp };
        vkGetPhysicalDeviceProperties2(devs[i], &p2);
        LOG("GPU %u: %s (type %d)", i, p2.properties.deviceName, (int)p2.properties.deviceType);
        char lbl[64];
        snprintf(lbl, sizeof lbl, "  GPU %u device UUID: ", i);
        print_uuid(lbl, idp.deviceUUID);
        if (pick < 0 && p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pick = (int)i;
    }
    if (want_gpu >= 0) {
        if ((uint32_t)want_gpu >= n) DIE("--gpu %d: only %u device(s)", want_gpu, n);
        pick = want_gpu;
    }
    if (pick < 0) pick = 0;
    g_phys = devs[pick];
    LOG("using GPU %d. The shim logs the OpenGL device UUID: it must match this one.", pick);

    /* Is an exportable opaque-fd image of our kind supported? */
    VkPhysicalDeviceExternalImageFormatInfo ext = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkPhysicalDeviceImageFormatInfo2 fi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &ext,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
    VkExternalImageFormatProperties efp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 fp = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &efp };
    VkResult r = vkGetPhysicalDeviceImageFormatProperties2(g_phys, &fi, &fp);
    if (r != VK_SUCCESS) DIE("RGBA8 optimal-tiling image with OPAQUE_FD export not supported (VkResult %d)", (int)r);
    VkExternalMemoryFeatureFlags f = efp.externalMemoryProperties.externalMemoryFeatures;
    LOG("opaque-fd image: exportable=%d importable=%d dedicated-only=%d", !!(f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT),
        !!(f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT), !!(f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT));
    if (!(f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) DIE("image memory is not exportable on this GPU");

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &ne, NULL);
    VkExtensionProperties *exts = calloc(ne ? ne : 1, sizeof *exts);
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &ne, exts);
    int has_fd = 0;
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(exts[i].extensionName, "VK_KHR_external_memory_fd")) has_fd = 1;
    int has_semfd = 0;
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(exts[i].extensionName, "VK_KHR_external_semaphore_fd")) has_semfd = 1;
    free(exts);
    if (!has_fd) DIE("VK_KHR_external_memory_fd not supported by this GPU/driver");

    /* Can binary semaphores be exported AND imported as opaque fds? (needed for GL_EXT_semaphore_fd) */
    VkPhysicalDeviceExternalSemaphoreInfo esi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
                                                  .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkExternalSemaphoreProperties esp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
    vkGetPhysicalDeviceExternalSemaphoreProperties(g_phys, &esi, &esp);
    int sem_exp = !!(esp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT);
    int sem_imp = !!(esp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
    g_sem_ok = has_semfd && sem_exp && sem_imp && !g_no_sem;
    LOG("opaque-fd semaphore: extension=%d exportable=%d importable=%d -> %s", has_semfd, sem_exp, sem_imp,
        g_no_sem ? "disabled by --no-semaphores" : (g_sem_ok ? "semaphore sync available" : "NOT available (the game will use glFinish + ack)"));

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, qf);
    int fam = -1;
    for (uint32_t i = 0; i < nq && fam < 0; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) fam = (int)i;
    for (uint32_t i = 0; i < nq && fam < 0; i++) if (qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) fam = (int)i;
    free(qf);
    if (fam < 0) DIE("no usable queue family");
    g_qfam = (uint32_t)fam;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qfam,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    const char *dext[] = { "VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = g_sem_ok ? 2u : 1u, .ppEnabledExtensionNames = dext };
    VKCHECK(vkCreateDevice(g_phys, &dci, NULL, &g_dev));
#define LOADD(name) name = (PFN_##name)pfnGetDeviceProcAddr(g_dev, #name); if (!name) DIE("missing " #name);
    VK_DEVICE_FUNCS(LOADD)
#undef LOADD
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    if (g_sem_ok) {
        pfnGetSemFd = (PFN_vkGetSemaphoreFdKHR)pfnGetDeviceProcAddr(g_dev, "vkGetSemaphoreFdKHR");
        pfnImportSemFd = (PFN_vkImportSemaphoreFdKHR)pfnGetDeviceProcAddr(g_dev, "vkImportSemaphoreFdKHR");
        if (!pfnGetSemFd || !pfnImportSemFd) { LOG("vkGetSemaphoreFdKHR / vkImportSemaphoreFdKHR missing: semaphores disabled"); g_sem_ok = 0; }
    }
}

/* One shared image: exportable memory, dedicated allocation, optimal tiling. Stage 3d: g_eyes of them (eye 0 = the single
 * image of the earlier stages); the hello offers all of them and the game uses as many as it was asked for. */
static void create_one_shared_image(int e)
{
    VkExternalMemoryImageCreateInfo emi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { g_w, g_h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VKCHECK(vkCreateImage(g_dev, &ii, NULL, &g_image[e]));

    VkMemoryDedicatedRequirements ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 mr = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &ded };
    VkImageMemoryRequirementsInfo2 mri = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = g_image[e] };
    vkGetImageMemoryRequirements2(g_dev, &mri, &mr);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    int mt = find_memory_type(&mp, mr.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) DIE("no device-local memory type for the shared image");

    VkMemoryDedicatedAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = g_image[e] };
    VkExportMemoryAllocateInfo ema = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &da,
                                      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ema,
                                .allocationSize = mr.memoryRequirements.size, .memoryTypeIndex = (uint32_t)mt };
    VKCHECK(vkAllocateMemory(g_dev, &ai, NULL, &g_image_mem[e]));
    VKCHECK(vkBindImageMemory(g_dev, g_image[e], g_image_mem[e], 0));
    /* The hello carries ONE memory size for every image, so identical images must give identical sizes. */
    if (e > 0 && ai.allocationSize != g_image_mem_size)
        DIE("eye %d image needs %llu bytes but eye 0 needs %llu: the hello can only carry one size", e,
            (unsigned long long)ai.allocationSize, (unsigned long long)g_image_mem_size);
    g_image_mem_size = ai.allocationSize;
    LOG("shared image %ux%u RGBA8, memory %llu bytes (memory type %d, dedicated requirement: %d)%s", g_w, g_h,
        (unsigned long long)g_image_mem_size, mt, (int)ded.requiresDedicatedAllocation, g_eyes > 1 ? (e ? " [eye 1]" : " [eye 0]") : "");
}

static void create_shared_image(void)
{
    for (int e = 0; e < g_eyes; e++) create_one_shared_image(e);
}

/* Host-visible readback buffer per eye + one command buffer, and the one-time UNDEFINED -> GENERAL transition. */
static void create_readback(void)
{
    VkDeviceSize bytes = (VkDeviceSize)g_w * g_h * 4;
    for (int e = 0; e < g_eyes; e++) {
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VKCHECK(vkCreateBuffer(g_dev, &bi, NULL, &g_buf[e]));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g_dev, g_buf[e], &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
        int mt = find_memory_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt < 0) DIE("no host-visible coherent memory type");
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
        VKCHECK(vkAllocateMemory(g_dev, &ai, NULL, &g_buf_mem[e]));
        VKCHECK(vkBindBufferMemory(g_dev, g_buf[e], g_buf_mem[e], 0));
        VKCHECK(vkMapMemory(g_dev, g_buf_mem[e], 0, VK_WHOLE_SIZE, 0, &g_buf_map[e]));
    }

    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qfam };
    VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &g_pool));
    VkCommandBufferAllocateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VKCHECK(vkAllocateCommandBuffers(g_dev, &ci, &g_cmd));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VKCHECK(vkCreateFence(g_dev, &fi, NULL, &g_fence));

    /* One-time transition to GENERAL. From then on the images stay in GENERAL: the GL side writes
     * them, we only read them (UNDEFINED as the old layout would allow the driver to discard them). */
    VkCommandBufferBeginInfo bb = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKCHECK(vkBeginCommandBuffer(g_cmd, &bb));
    VkImageMemoryBarrier b[MAX_EYES];
    for (int e = 0; e < g_eyes; e++)
        b[e] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_image[e],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, (uint32_t)g_eyes, b);
    VKCHECK(vkEndCommandBuffer(g_cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_cmd };
    VKCHECK(vkQueueSubmit(g_queue, 1, &si, g_fence));
    VKCHECK(vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX));
    VKCHECK(vkResetFences(g_dev, 1, &g_fence));
}

/* Record the image -> host buffer copies of the first `eyes` images into g_cmd (barrier from "the other API wrote it"
 * included). One command buffer for the whole pair. */
static void record_readback_cmd(int eyes)
{
    VKCHECK(vkResetCommandBuffer(g_cmd, 0));
    VkCommandBufferBeginInfo bb = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKCHECK(vkBeginCommandBuffer(g_cmd, &bb));
    /* Make the other API's writes visible to the copies. */
    VkImageMemoryBarrier b[MAX_EYES];
    for (int e = 0; e < eyes; e++)
        b[e] = (VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_image[e],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, (uint32_t)eyes, b);
    VkBufferMemoryBarrier bm[MAX_EYES];
    for (int e = 0; e < eyes; e++) {
        VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { g_w, g_h, 1 } };
        vkCmdCopyImageToBuffer(g_cmd, g_image[e], VK_IMAGE_LAYOUT_GENERAL, g_buf[e], 1, &c);
        bm[e] = (VkBufferMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = g_buf[e], .size = VK_WHOLE_SIZE };
    }
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, (uint32_t)eyes, bm, 0, NULL);
    VKCHECK(vkEndCommandBuffer(g_cmd));
}

/* Legacy (glFinish + ack) frame: copy and wait on the CPU. */
static void read_back_image(int eyes)
{
    record_readback_cmd(eyes);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_cmd };
    VKCHECK(vkQueueSubmit(g_queue, 1, &si, g_fence));
    VKCHECK(vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX));
    VKCHECK(vkResetFences(g_dev, 1, &g_fence));
}

/* ---------------------------------------------------- stage 3a: semaphore-synchronised frames */
/* Wait on a fence, but never forever: if the game's GL side never signalled gl_ready (the game died
 * between the signal and the message) this queue is stuck behind a wait that cannot complete, and
 * the only cure is a fresh helper. Say so instead of hanging silently. */
static void wait_fence_or_die(const char *what)
{
    VkResult r = vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, 5000000000ull);
    if (r == VK_TIMEOUT) DIE("%s: no completion after 5 s. The game's GL side never signalled the semaphore (the game died or the "
                             "GL_EXT_semaphore_fd path is broken on this driver). Restart this helper; start the game with sync=finish.", what);
    VKCHECK(r);
    VKCHECK(vkResetFences(g_dev, 1, &g_fence));
}

/* Submit one frame (one PAIR when eyes == 2): wait gl_ready; optionally copy the images to the host buffers; signal
 * vk_done. ONE wait and ONE signal cover both eyes, so a pair is never half-consumed. Returns after vkQueueSubmit
 * (the GPU may still be working): that is what the ack promises. */
static void submit_sem_frame(int save, int eyes)
{
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (save) record_readback_cmd(eyes);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &g_sem_gl_ready, .pWaitDstStageMask = &ws,
        .commandBufferCount = save ? 1u : 0u, .pCommandBuffers = save ? &g_cmd : NULL,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &g_sem_vk_done };
    VKCHECK(vkQueueSubmit(g_queue, 1, &si, save ? g_fence : VK_NULL_HANDLE));
}

static void create_sync_semaphores(int exportable)
{
    VkExportSemaphoreCreateInfo ex = { .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                       .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = exportable ? &ex : NULL };
    VKCHECK(vkCreateSemaphore(g_dev, &sci, NULL, &g_sem_gl_ready));
    VKCHECK(vkCreateSemaphore(g_dev, &sci, NULL, &g_sem_vk_done));
    g_sem_active = 1;
}

static void destroy_sync_semaphores(void)
{
    if (!g_sem_active) return;
    vkDeviceWaitIdle(g_dev);
    vkDestroySemaphore(g_dev, g_sem_gl_ready, NULL);
    vkDestroySemaphore(g_dev, g_sem_vk_done, NULL);
    g_sem_gl_ready = g_sem_vk_done = VK_NULL_HANDLE;
    g_sem_active = 0;
}

static int export_semaphore_fd(VkSemaphore sem)
{
    VkSemaphoreGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = sem,
                                  .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
    int fd = -1;
    VKCHECK(pfnGetSemFd(g_dev, &gi, &fd));
    return fd;
}

/* Test-side helpers: an empty submit that only signals / only waits (stands in for the GL side). */
static void test_signal(VkSemaphore sem)
{
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1, .pSignalSemaphores = &sem };
    VKCHECK(vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE));
}
static void test_wait(VkSemaphore sem, const char *what)
{
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem, .pWaitDstStageMask = &ws };
    VKCHECK(vkQueueSubmit(g_queue, 1, &si, g_fence));
    wait_fence_or_die(what);
}

/* --selftest-sync: the exact frame cycle of the game, with this process playing the GL side. If the
 * device can export/import semaphores, the "GL side" works through IMPORTED copies of the exported
 * fds, so the fd-sharing itself is exercised too. */
static int selftest_sync(void)
{
    int via_fds = g_sem_ok;
    LOG("selftest: %s", via_fds ? "semaphores are exported as fds and re-imported (the GL side is simulated through the imports)"
                                : "this device cannot share semaphores by fd; testing the submit logic with local semaphores only");
    create_sync_semaphores(via_fds);
    VkSemaphore gl_sig = g_sem_gl_ready, gl_wait = g_sem_vk_done;   /* what the simulated GL side uses */
    if (via_fds) {
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VKCHECK(vkCreateSemaphore(g_dev, &sci, NULL, &gl_sig));
        VKCHECK(vkCreateSemaphore(g_dev, &sci, NULL, &gl_wait));
        VkImportSemaphoreFdInfoKHR i1 = { .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = gl_sig,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = export_semaphore_fd(g_sem_gl_ready) };
        VkImportSemaphoreFdInfoKHR i2 = { .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = gl_wait,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = export_semaphore_fd(g_sem_vk_done) };
        VKCHECK(pfnImportSemFd(g_dev, &i1));
        VKCHECK(pfnImportSemFd(g_dev, &i2));
    }
    const int cycles = 50;
    for (int i = 0; i < cycles; i++) {
        test_signal(gl_sig);                        /* GL: blit done, signal(gl_ready) */
        submit_sem_frame(i == 0, g_eyes);           /* helper: wait(gl_ready) -> copy (all eyes) -> signal(vk_done) */
        if (i == 0) wait_fence_or_die("selftest frame 0 (copy)");
        test_wait(gl_wait, "selftest: GL-side wait(vk_done)");   /* GL: next frame's wait(vk_done) */
    }
    vkDeviceWaitIdle(g_dev);
    if (via_fds) { vkDestroySemaphore(g_dev, gl_sig, NULL); vkDestroySemaphore(g_dev, gl_wait, NULL); }
    destroy_sync_semaphores();
    LOG("selftest: PASS (%d signal/wait cycles; fd export+import %s)", cycles, via_fds ? "verified" : "NOT tested on this device");
    return 0;
}

/* Write g_buf_map as a binary PPM; also report how much of it is non-black. */
static void write_ppm(const char *path, uint32_t tick, int eye)
{
    const uint8_t *src = (const uint8_t *)g_buf_map[eye];
    size_t px = (size_t)g_w * g_h, lit = 0;
    uint8_t *rgb = malloc(px * 3);
    if (!rgb) return;
    for (size_t i = 0; i < px; i++) {
        rgb[i * 3 + 0] = src[i * 4 + 0];
        rgb[i * 3 + 1] = src[i * 4 + 1];
        rgb[i * 3 + 2] = src[i * 4 + 2];
        if (src[i * 4] | src[i * 4 + 1] | src[i * 4 + 2]) lit++;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", g_w, g_h);
        fwrite(rgb, 3, px, f);
        fclose(f);
        LOG("frame (shim tick %u): wrote %s, %.1f%% of pixels non-black", tick, path, 100.0 * (double)lit / (double)px);
        if (lit == 0) LOG("  all black: the blit may have read an empty buffer (see the shim log: 'xfer' lines)");
    } else {
        LOG("cannot write %s: %s", path, strerror(errno));
    }
    free(rgb);
}

/* ------------------------------------------------------------------ socket */
static int send_hello_with_fds(int client, const int *fds, int nfds, uint32_t caps)
{
    struct bmvr_xfer_hello h = { .magic = BMVR_XFER_MAGIC_HELLO, .version = BMVR_XFER_VERSION, .width = g_w, .height = g_h,
        .gl_internal_format = 0x8058 /* GL_RGBA8 */, .dedicated = 1, .optimal_tiling = 1, .caps = caps,
        .eye_count = (uint32_t)g_eyes, .memory_size = g_image_mem_size };
    struct iovec iov = { .iov_base = &h, .iov_len = sizeof h };
    char ctl[CMSG_SPACE(4 * sizeof(int))];   /* up to 2 memory fds + 2 semaphore fds */
    memset(ctl, 0, sizeof ctl);
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = CMSG_SPACE((size_t)nfds * sizeof(int)) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN((size_t)nfds * sizeof(int));
    memcpy(CMSG_DATA(c), fds, (size_t)nfds * sizeof(int));
    return sendmsg(client, &m, MSG_NOSIGNAL) == (ssize_t)sizeof h ? 0 : -1;
}

static int recv_all(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, (char *)buf + got, len - got, 0);
        if (n == 0) return -1;
        if (n < 0) { if (errno == EINTR && !g_stop) continue; return -1; }
        got += (size_t)n;
    }
    return 0;
}

/* Write one eye's snapshot from its readback buffer and say what it is. One eye (the earlier stages' format):
 * bmvr_snapshot_<n>_<what>_flip<f>.ppm. A pair: bmvr_snapshot_<n>_eye<0|1>_<what>_flip<f>.ppm. */
static void save_one_snapshot(const char *gamedir, int n, int eye, int pair, uint32_t tick, const char *raw_name, uint32_t flags,
                              uint32_t src_w, uint32_t src_h, uint32_t gl_name, int use_sem)
{
    char nm[25], what[sizeof nm + 1], path[1024 + 128], tag[16] = "";
    memcpy(nm, raw_name, 24);
    nm[24] = 0;                                        /* never trust the peer's terminator */
    size_t k = 0;
    for (const char *q = nm; *q && k < sizeof what - 1; q++)   /* file-name safe */
        what[k++] = ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-') ? *q : '_';
    what[k] = 0;
    if (!what[0]) strcpy(what, "unnamed");
    if (pair) snprintf(tag, sizeof tag, "eye%d_", eye);
    snprintf(path, sizeof path, "%s/bmvr_snapshot_%d_%s%s_flip%d.ppm", gamedir, n, tag, what, (flags & BMVR_XFER_FLAG_FLIPPED) ? 1 : 0);
    write_ppm(path, tick, eye);
    LOG("  %ssource '%s' (%s) was %ux%u, GL object %u, y-flipped by the shim: %s; scaled into the shared image; synced by %s",
        pair ? (eye ? "eye 1: " : "eye 0: ") : "", nm, (flags & BMVR_XFER_FLAG_WINDOW) ? "window" : "render target", src_w, src_h, gl_name,
        (flags & BMVR_XFER_FLAG_FLIPPED) ? "yes" : "no", use_sem ? "GPU semaphores" : "glFinish + ack");
}

/* A frame message (one eye or a pair, already validated: 1 <= eye_count <= g_eyes) -> snapshot file(s). */
static void save_snapshot(const char *gamedir, const struct bmvr_xfer_frame *fr, int n)
{
    int pair = fr->eye_count > 1, sem = (fr->flags & BMVR_XFER_FLAG_SEMAPHORE) != 0;
    save_one_snapshot(gamedir, n, 0, pair, fr->tick, fr->name, fr->flags, fr->src_w, fr->src_h, fr->gl_name, sem);
    if (pair) save_one_snapshot(gamedir, n, 1, pair, fr->tick, fr->eye1_name, fr->eye1_flags, fr->eye1_src_w, fr->eye1_src_h, fr->eye1_gl_name, sem);
}

static void serve(const char *gamedir, int max_snaps)
{
    char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path) + 256];
    snprintf(sock_path, sizeof sock_path, "%s/%s", gamedir, BMVR_XFER_SOCKET_NAME);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(sock_path) >= sizeof sa.sun_path)
        DIE("socket path too long for a unix socket (%zu >= %zu): %s", strlen(sock_path), sizeof sa.sun_path, sock_path);
    strcpy(sa.sun_path, sock_path);
    unlink(sock_path);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0 || bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(srv, 1) < 0)
        DIE("cannot listen on %s: %s", sock_path, strerror(errno));
    LOG("listening on %s", sock_path);
    LOG("now start Black Mesa from Steam (with the bmvr-run.sh launch option). Ctrl-C stops the helper.");
    LOG("synchronisation offered to the game: %s", g_sem_ok ? "GPU semaphores (the game opts in with sync=sem in bmvr_target.txt), otherwise glFinish + ack"
                                                            : "glFinish + ack only");

    int snaps = 0;
    while (!g_stop) {
        int cl = accept(srv, NULL, NULL);
        if (cl < 0) { if (errno == EINTR) continue; break; }
        int fds[4] = { -1, -1, -1, -1 }, nfds = g_eyes;
        for (int e = 0; e < g_eyes; e++) {
            VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = g_image_mem[e],
                                       .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
            VKCHECK(vkGetMemoryFdKHR(g_dev, &gi, &fds[e]));   /* a fresh fd per client and image; the receiver owns it */
        }
        if (g_sem_ok) {
            /* Fresh semaphores per client: a previous game may have left one of them signalled. ONE pair serves both eyes. */
            destroy_sync_semaphores();
            create_sync_semaphores(1);
            fds[nfds++] = export_semaphore_fd(g_sem_gl_ready);
            fds[nfds++] = export_semaphore_fd(g_sem_vk_done);
        }
        int hello_ok = send_hello_with_fds(cl, fds, nfds, g_sem_ok ? BMVR_XFER_CAP_SEMAPHORES : 0) == 0;
        for (int i = 0; i < nfds; i++) close(fds[i]);     /* our copies; the game holds its own */
        if (!hello_ok) { LOG("could not send the memory fd to the game"); close(cl); continue; }
        LOG("game connected; %s sent (%ux%u)%s", g_eyes > 1 ? "two shared images (eye 0, eye 1)" : "shared image", g_w, g_h, g_sem_ok ? ", with two GPU semaphores" : "");

        for (;;) {
            struct bmvr_xfer_frame fr;
            if (recv_all(cl, &fr, sizeof fr) < 0) break;
            if (fr.magic != BMVR_XFER_MAGIC_FRAME) { LOG("bad frame message, dropping the game connection"); break; }
            if (fr.eye_count < 1 || fr.eye_count > (uint32_t)g_eyes) {
                LOG("frame with %u eye(s) but this helper serves %d (start it with --eyes 2 for pairs); dropping the game connection", fr.eye_count, g_eyes);
                break;
            }
            int use_sem = (fr.flags & BMVR_XFER_FLAG_SEMAPHORE) != 0;
            if (use_sem && !g_sem_active) { LOG("frame wants semaphore sync but none were offered; dropping the game connection"); break; }
            int save = snaps < max_snaps;
            struct bmvr_xfer_ack ack = { .magic = BMVR_XFER_MAGIC_ACK, .status = 0 };
            if (use_sem) {
                submit_sem_frame(save, (int)fr.eye_count);   /* wait(gl_ready) [-> copy] -> signal(vk_done), no CPU wait; one pair = one wait + one signal */
                if (send(cl, &ack, sizeof ack, MSG_NOSIGNAL) != (ssize_t)sizeof ack) break;   /* "submitted" */
                if (save) {
                    wait_fence_or_die("frame copy");
                    save_snapshot(gamedir, &fr, snaps++);
                }
            } else {
                if (save) {
                    read_back_image((int)fr.eye_count);
                    save_snapshot(gamedir, &fr, snaps++);
                }
                if (send(cl, &ack, sizeof ack, MSG_NOSIGNAL) != (ssize_t)sizeof ack) break;
            }
            if (snaps == max_snaps) { LOG("wrote %d snapshot(s); further frames are acknowledged but not saved", snaps); snaps++; }
        }
        close(cl);
        LOG("game disconnected");
    }
    close(srv);
    unlink(sock_path);
}

int main(int argc, char **argv)
{
    char gamedir[900] = "";
    int want_gpu = -1, max_snaps = 5, selftest = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &g_w, &g_h) != 2 || !g_w || !g_h || g_w > 8192 || g_h > 8192) DIE("bad --size, use e.g. 1920x1080");
        } else if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
            want_gpu = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--snapshots") && i + 1 < argc) {
            max_snaps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--eyes") && i + 1 < argc) {
            g_eyes = atoi(argv[++i]);
            if (g_eyes < 1 || g_eyes > MAX_EYES) DIE("bad --eyes, use 1 or 2");
        } else if (!strcmp(argv[i], "--no-semaphores")) {
            g_no_sem = 1;
        } else if (!strcmp(argv[i], "--selftest-sync")) {
            selftest = 1;
        } else if (argv[i][0] != '-' && !gamedir[0]) {
            snprintf(gamedir, sizeof gamedir, "%s", argv[i]);
        } else {
            fprintf(stderr, "usage: %s [game folder] [--size WxH] [--gpu N] [--snapshots N] [--eyes 1|2] [--no-semaphores] | --selftest-sync\n", argv[0]);
            return 2;
        }
    }
    if (!gamedir[0]) { /* default: the folder this program lives in */
        ssize_t n = readlink("/proc/self/exe", gamedir, sizeof gamedir - 1);
        if (n <= 0) DIE("give the game folder as the first argument");
        gamedir[n] = 0;
        char *s = strrchr(gamedir, '/');
        if (s) *s = 0;
    }
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (!selftest) {
        LOG("game folder: %s", gamedir);
        /* Before touching the GPU: a second helper must not take the socket over from a running one. */
        char err[1400]; long holder = 0;
        if (bmvr_acquire_helper_lock(gamedir, err, sizeof err, &holder) < 0) {
            if (holder > 0) DIE("%s (pid %ld). Stop it first (Ctrl-C in its terminal, or: kill %ld).", err, holder, holder);
            DIE("%s", err);
        }
    }
    vulkan_init(want_gpu);
    create_shared_image();
    create_readback();
    if (selftest) return selftest_sync();
    serve(gamedir, max_snaps);
    destroy_sync_semaphores();
    vkDeviceWaitIdle(g_dev);
    return 0;
}
