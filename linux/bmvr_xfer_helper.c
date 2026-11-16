/*
 * Black Mesa VR - native Linux port, stage 2a/2b: 64-bit helper for the GPU hand-off test.
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
 * Usage:  bmvr_xfer_helper [game folder] [--size WxH] [--gpu N] [--snapshots N]
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
    X(vkGetPhysicalDeviceImageFormatProperties2) X(vkCreateDevice) X(vkGetDeviceProcAddr)

#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements2) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) \
    X(vkGetMemoryFdKHR) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) \
    X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkDeviceWaitIdle)

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
static VkImage g_image;
static VkDeviceMemory g_image_mem;
static VkDeviceSize g_image_mem_size;
static VkBuffer g_buf;
static VkDeviceMemory g_buf_mem;
static void *g_buf_map;
static VkCommandPool g_pool;
static VkCommandBuffer g_cmd;
static VkFence g_fence;
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
    free(exts);
    if (!has_fd) DIE("VK_KHR_external_memory_fd not supported by this GPU/driver");

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
    const char *dext[] = { "VK_KHR_external_memory_fd" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
    VKCHECK(vkCreateDevice(g_phys, &dci, NULL, &g_dev));
#define LOADD(name) name = (PFN_##name)pfnGetDeviceProcAddr(g_dev, #name); if (!name) DIE("missing " #name);
    VK_DEVICE_FUNCS(LOADD)
#undef LOADD
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
}

/* The shared image: exportable memory, dedicated allocation, optimal tiling. */
static void create_shared_image(void)
{
    VkExternalMemoryImageCreateInfo emi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { g_w, g_h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VKCHECK(vkCreateImage(g_dev, &ii, NULL, &g_image));

    VkMemoryDedicatedRequirements ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 mr = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &ded };
    VkImageMemoryRequirementsInfo2 mri = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = g_image };
    vkGetImageMemoryRequirements2(g_dev, &mri, &mr);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    int mt = find_memory_type(&mp, mr.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) DIE("no device-local memory type for the shared image");

    VkMemoryDedicatedAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = g_image };
    VkExportMemoryAllocateInfo ema = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &da,
                                      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ema,
                                .allocationSize = mr.memoryRequirements.size, .memoryTypeIndex = (uint32_t)mt };
    VKCHECK(vkAllocateMemory(g_dev, &ai, NULL, &g_image_mem));
    VKCHECK(vkBindImageMemory(g_dev, g_image, g_image_mem, 0));
    g_image_mem_size = ai.allocationSize;
    LOG("shared image %ux%u RGBA8, memory %llu bytes (memory type %d, dedicated requirement: %d)", g_w, g_h,
        (unsigned long long)g_image_mem_size, mt, (int)ded.requiresDedicatedAllocation);
}

/* Host-visible readback buffer + command buffer, and the one-time UNDEFINED -> GENERAL transition. */
static void create_readback(void)
{
    VkDeviceSize bytes = (VkDeviceSize)g_w * g_h * 4;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VKCHECK(vkCreateBuffer(g_dev, &bi, NULL, &g_buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev, g_buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    int mt = find_memory_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) DIE("no host-visible coherent memory type");
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    VKCHECK(vkAllocateMemory(g_dev, &ai, NULL, &g_buf_mem));
    VKCHECK(vkBindBufferMemory(g_dev, g_buf, g_buf_mem, 0));
    VKCHECK(vkMapMemory(g_dev, g_buf_mem, 0, VK_WHOLE_SIZE, 0, &g_buf_map));

    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qfam };
    VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &g_pool));
    VkCommandBufferAllocateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VKCHECK(vkAllocateCommandBuffers(g_dev, &ci, &g_cmd));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VKCHECK(vkCreateFence(g_dev, &fi, NULL, &g_fence));

    /* One-time transition to GENERAL. From then on the image stays in GENERAL: the GL side writes
     * it, we only read it (UNDEFINED as the old layout would allow the driver to discard it). */
    VkCommandBufferBeginInfo bb = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKCHECK(vkBeginCommandBuffer(g_cmd, &bb));
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VKCHECK(vkEndCommandBuffer(g_cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_cmd };
    VKCHECK(vkQueueSubmit(g_queue, 1, &si, g_fence));
    VKCHECK(vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX));
    VKCHECK(vkResetFences(g_dev, 1, &g_fence));
}

/* Copy the shared image into g_buf_map (RGBA8, top row first). */
static void read_back_image(void)
{
    VKCHECK(vkResetCommandBuffer(g_cmd, 0));
    VkCommandBufferBeginInfo bb = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKCHECK(vkBeginCommandBuffer(g_cmd, &bb));
    /* Make the other API's writes visible to the copy. */
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { g_w, g_h, 1 } };
    vkCmdCopyImageToBuffer(g_cmd, g_image, VK_IMAGE_LAYOUT_GENERAL, g_buf, 1, &c);
    VkBufferMemoryBarrier bm = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = g_buf, .size = VK_WHOLE_SIZE };
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bm, 0, NULL);
    VKCHECK(vkEndCommandBuffer(g_cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_cmd };
    VKCHECK(vkQueueSubmit(g_queue, 1, &si, g_fence));
    VKCHECK(vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX));
    VKCHECK(vkResetFences(g_dev, 1, &g_fence));
}

/* Write g_buf_map as a binary PPM; also report how much of it is non-black. */
static void write_ppm(const char *path, uint32_t tick)
{
    const uint8_t *src = (const uint8_t *)g_buf_map;
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
static int send_hello_with_fd(int client, int memfd)
{
    struct bmvr_xfer_hello h = { .magic = BMVR_XFER_MAGIC_HELLO, .version = BMVR_XFER_VERSION, .width = g_w, .height = g_h,
        .gl_internal_format = 0x8058 /* GL_RGBA8 */, .dedicated = 1, .optimal_tiling = 1, .memory_size = g_image_mem_size };
    struct iovec iov = { .iov_base = &h, .iov_len = sizeof h };
    char ctl[CMSG_SPACE(sizeof(int))];
    memset(ctl, 0, sizeof ctl);
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &memfd, sizeof(int));
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

    int snaps = 0;
    while (!g_stop) {
        int cl = accept(srv, NULL, NULL);
        if (cl < 0) { if (errno == EINTR) continue; break; }
        VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = g_image_mem,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        int memfd = -1;
        VKCHECK(vkGetMemoryFdKHR(g_dev, &gi, &memfd));   /* a fresh fd per client; the receiver owns it */
        if (send_hello_with_fd(cl, memfd) < 0) { LOG("could not send the memory fd to the game"); close(memfd); close(cl); continue; }
        close(memfd); /* our copy; the game holds its own */
        LOG("game connected; shared image sent (%ux%u)", g_w, g_h);

        for (;;) {
            struct bmvr_xfer_frame fr;
            if (recv_all(cl, &fr, sizeof fr) < 0) break;
            if (fr.magic != BMVR_XFER_MAGIC_FRAME) { LOG("bad frame message, dropping the game connection"); break; }
            if (snaps < max_snaps) {
                read_back_image();
                char what[sizeof fr.name + 1], path[1024 + 128];
                fr.name[sizeof fr.name - 1] = 0;                 /* never trust the peer's terminator */
                size_t k = 0;
                for (const char *q = fr.name; *q && k < sizeof what - 1; q++)   /* file-name safe */
                    what[k++] = ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-') ? *q : '_';
                what[k] = 0;
                if (!what[0]) strcpy(what, "unnamed");
                snprintf(path, sizeof path, "%s/bmvr_snapshot_%d_%s_flip%d.ppm", gamedir, snaps, what, (fr.flags & BMVR_XFER_FLAG_FLIPPED) ? 1 : 0);
                write_ppm(path, fr.tick);
                LOG("  source '%s' (%s) was %ux%u, GL object %u, y-flipped by the shim: %s; scaled into the shared image",
                    fr.name, (fr.flags & BMVR_XFER_FLAG_WINDOW) ? "window" : "render target", fr.src_w, fr.src_h, fr.gl_name,
                    (fr.flags & BMVR_XFER_FLAG_FLIPPED) ? "yes" : "no");
                snaps++;
            }
            struct bmvr_xfer_ack ack = { .magic = BMVR_XFER_MAGIC_ACK, .status = 0 };
            if (send(cl, &ack, sizeof ack, MSG_NOSIGNAL) != (ssize_t)sizeof ack) break;
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
    int want_gpu = -1, max_snaps = 5;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &g_w, &g_h) != 2 || !g_w || !g_h || g_w > 8192 || g_h > 8192) DIE("bad --size, use e.g. 1920x1080");
        } else if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
            want_gpu = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--snapshots") && i + 1 < argc) {
            max_snaps = atoi(argv[++i]);
        } else if (argv[i][0] != '-' && !gamedir[0]) {
            snprintf(gamedir, sizeof gamedir, "%s", argv[i]);
        } else {
            fprintf(stderr, "usage: %s [game folder] [--size WxH] [--gpu N] [--snapshots N]\n", argv[0]);
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

    LOG("game folder: %s", gamedir);
    vulkan_init(want_gpu);
    create_shared_image();
    create_readback();
    serve(gamedir, max_snaps);
    vkDeviceWaitIdle(g_dev);
    return 0;
}
