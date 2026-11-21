/*
 * Stage 4a, X2: a stand-in for the 32-bit shim, for testing `bmvr_xfer_helper --sink xr` WITHOUT the game, GL or a headset.
 *
 *   cc -O2 -Wall -o xr_client tests/linux/xr_client.c -lvulkan
 *   xr_client <socket path> [--seconds S] [--fps F] [--reconnect] [--max-eyes 1|2]
 *
 * It speaks protocol v4 exactly like the shim (hello + fds in, frame out, ack in), but with Vulkan instead of GL: it imports the
 * memory fd(s) of the shared image(s) into its own VkDevice, writes a recognisable pattern and sends one frame message per pair.
 * Every pair carries its own number in the pixels: pixel 0 of the first row = { tick & 255, tick >> 8, 0xE0 | eye, 0xFF }, pixel 15
 * = { 0xAB, 0xCD, 0xE0 | eye, 0xFF }; the rest of the image is a flat colour per eye. The helper's --xr-check reads the same pixels
 * back at every stage, so a test can compare "tick in the message" with "tick in the pixels" for both eyes.
 *
 * Only the glFinish + ack sync is possible here (no GL, and lavapipe has no semaphore export): the client waits for its own writes
 * (vkQueueWaitIdle), sends the frame, waits for the ack. The semaphore path needs the real game on a real GPU.
 *
 * "The game must be unaffected by the helper's death": a missing ack while connected for more than 5 s is reported as a HANG (exit 3).
 * With --reconnect the client behaves like the shim: it reconnects after the helper vanished and carries on counting.
 *
 * Output (stdout, one line at the end): client: sent=N acked=N connects=C last_tick=T eyes=E size=WxH hang=0|1
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include "../../linux/bmvr_xfer.h"

#define CK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { fprintf(stderr, "client: %s failed: VkResult %d\n", #call, (int)r_); return -1; } } while (0)

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkCommandPool pool;
static VkCommandBuffer cb;
static PFN_vkGetMemoryFdPropertiesKHR dummy_unused;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }

static int vk_start(void)
{
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "xr_client", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, &phys);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) { fprintf(stderr, "client: no Vulkan device\n"); return -1; }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    qfam = 0;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    const char *ext[] = { "VK_KHR_external_memory_fd" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext };
    CK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CK(vkAllocateCommandBuffers(dev, &ai, &cb));
    (void)dummy_unused;
    return 0;
}

static int find_mt(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

struct session {
    int sock;
    struct bmvr_xfer_hello hello;
    int eyes;
    VkImage img[2];
    VkDeviceMemory mem[2];
    VkBuffer stg;
    VkDeviceMemory stg_mem;
    uint8_t *map;
    VkDeviceSize stg_size;
};

/* hello + fds from the helper. Returns 0 on success. */
static int receive_hello(int sock, struct bmvr_xfer_hello *h, int fds[4], int *nfds)
{
    struct iovec iov = { .iov_base = h, .iov_len = sizeof *h };
    char ctl[CMSG_SPACE(4 * sizeof(int))];
    memset(ctl, 0, sizeof ctl);
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
    ssize_t n = recvmsg(sock, &m, 0);
    if (n != (ssize_t)sizeof *h) return -1;
    *nfds = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            memcpy(fds, CMSG_DATA(c), (size_t)cnt * sizeof(int));
            *nfds = cnt;
        }
    return (h->magic == BMVR_XFER_MAGIC_HELLO && h->version == BMVR_XFER_VERSION) ? 0 : -1;
}

static int session_open(struct session *s, int sock, int max_eyes)
{
    memset(s, 0, sizeof *s);
    s->sock = sock;
    int fds[4], nfds = 0;
    if (receive_hello(sock, &s->hello, fds, &nfds) < 0) { fprintf(stderr, "client: bad or missing hello\n"); return -1; }
    s->eyes = (int)s->hello.eye_count < max_eyes ? (int)s->hello.eye_count : max_eyes;
    if (nfds < (int)s->hello.eye_count) { fprintf(stderr, "client: hello has %d fd(s) for %u eye(s)\n", nfds, s->hello.eye_count); return -1; }
    uint32_t w = s->hello.width, h = s->hello.height;
    for (int e = 0; e < (int)s->hello.eye_count; e++) {
        if (e >= s->eyes) { close(fds[e]); continue; }
        VkExternalMemoryImageCreateInfo emi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        CK(vkCreateImage(dev, &ii, NULL, &s->img[e]));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, s->img[e], &mr);
        int mt = find_mt(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); /* the exporter picked the first device-local type too */
        if (mt < 0) { fprintf(stderr, "client: no device-local memory type\n"); return -1; }
        VkMemoryDedicatedAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = s->img[e] };
        VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &da, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fds[e] };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = s->hello.memory_size, .memoryTypeIndex = (uint32_t)mt };
        CK(vkAllocateMemory(dev, &mai, NULL, &s->mem[e])); /* takes ownership of the fd on success */
        CK(vkBindImageMemory(dev, s->img[e], s->mem[e], 0));
    }
    for (int i = (int)s->hello.eye_count; i < nfds; i++) close(fds[i]); /* semaphores: not usable here */

    /* staging buffer for the whole image (flat colour per eye, uploaded once) + the pattern row */
    s->stg_size = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = s->stg_size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CK(vkCreateBuffer(dev, &bi, NULL, &s->stg));
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(dev, s->stg, &br);
    int hm = find_mt(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (hm < 0) return -1;
    VkMemoryAllocateInfo bai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = br.size, .memoryTypeIndex = (uint32_t)hm };
    CK(vkAllocateMemory(dev, &bai, NULL, &s->stg_mem));
    CK(vkBindBufferMemory(dev, s->stg, s->stg_mem, 0));
    void *p = NULL;
    CK(vkMapMemory(dev, s->stg_mem, 0, VK_WHOLE_SIZE, 0, &p));
    s->map = p;
    return 0;
}

static void session_close(struct session *s)
{
    if (dev) vkDeviceWaitIdle(dev);
    for (int e = 0; e < 2; e++) {
        if (s->img[e]) vkDestroyImage(dev, s->img[e], NULL);
        if (s->mem[e]) vkFreeMemory(dev, s->mem[e], NULL);
    }
    if (s->stg) vkDestroyBuffer(dev, s->stg, NULL);
    if (s->stg_mem) vkFreeMemory(dev, s->stg_mem, NULL);
    if (s->sock >= 0) close(s->sock);
    memset(s, 0, sizeof *s);
    s->sock = -1;
}

/* Write the pair for `tick` into the imported images: whole image flat colour (first frame only), then the pattern pixels. */
static int write_pair(struct session *s, uint32_t tick, int first)
{
    uint32_t w = s->hello.width, h = s->hello.height;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    for (int e = 0; e < s->eyes; e++) {
        if (first) { /* flat field, different per eye */
            for (size_t i = 0; i < (size_t)w * h; i++) { s->map[i * 4] = e ? 120 : 20; s->map[i * 4 + 1] = 40; s->map[i * 4 + 2] = e ? 30 : 140; s->map[i * 4 + 3] = 255; }
        }
        uint8_t *row = s->map; /* first row: the tick pixel and the marker pixel (the rest of the first row keeps the field colour) */
        row[0] = (uint8_t)(tick & 255); row[1] = (uint8_t)((tick >> 8) & 255); row[2] = (uint8_t)(0xE0 | e); row[3] = 0xFF;
        row[60] = 0xAB; row[61] = 0xCD; row[62] = (uint8_t)(0xE0 | e); row[63] = 0xFF;
        CK(vkResetCommandBuffer(cb, 0));
        CK(vkBeginCommandBuffer(cb, &bi));
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = s->img[e],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = first ? (VkExtent3D){ w, h, 1 } : (VkExtent3D){ 16, 1, 1 } };
        vkCmdCopyBufferToImage(cb, s->stg, s->img[e], VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        VkImageMemoryBarrier z = b;
        z.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; z.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; z.oldLayout = z.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &z);
        CK(vkEndCommandBuffer(cb));
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        CK(vkQueueWaitIdle(queue)); /* sync=finish: our writes are complete before the frame message */
        first = 0; /* the flat field of eye 1 must be written with a full copy too: handled by `first` per call, so redo for eye 1 */
        if (e == 0 && s->eyes > 1) first = 1; /* eye 1 still needs its full upload (its layout is UNDEFINED as well) */
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <socket path> [--seconds S] [--fps F] [--reconnect] [--max-eyes 1|2]\n", argv[0]); return 2; }
    const char *path = argv[1];
    double seconds = 5, fps = 60;
    int reconnect = 0, max_eyes = 2;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--reconnect")) reconnect = 1;
        else if (!strcmp(argv[i], "--max-eyes") && i + 1 < argc) max_eyes = atoi(argv[++i]);
    }
    signal(SIGPIPE, SIG_IGN);
    if (vk_start() < 0) return 2;

    uint64_t sent = 0, acked = 0;
    uint32_t tick = 0;
    int connects = 0, hang = 0, eyes_seen = 0;
    uint32_t w_seen = 0, h_seen = 0;
    double t_end = now_s() + seconds;
    struct session s = { .sock = -1 };

    while (now_s() < t_end && !hang) {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
        if (connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0) {
            close(sock);
            if (!reconnect && connects == 0) { fprintf(stderr, "client: cannot connect to %s: %s\n", path, strerror(errno)); return 2; }
            struct timespec nap = { 0, 100 * 1000 * 1000 };
            nanosleep(&nap, NULL);
            if (!reconnect) break;
            continue;
        }
        struct timeval tv = { .tv_sec = 5 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (session_open(&s, sock, max_eyes) < 0) { session_close(&s); if (!reconnect) return 2; continue; }
        connects++;
        eyes_seen = s.eyes; w_seen = s.hello.width; h_seen = s.hello.height;
        int first = 1;
        while (now_s() < t_end) {
            tick++;
            if (write_pair(&s, tick, first) < 0) { session_close(&s); return 2; }
            first = 0;
            struct bmvr_xfer_frame fr;
            memset(&fr, 0, sizeof fr);
            fr.magic = BMVR_XFER_MAGIC_FRAME; fr.tick = tick; fr.src_w = fr.eye1_src_w = s.hello.width; fr.src_h = fr.eye1_src_h = s.hello.height;
            fr.flags = 0; fr.eye_count = (uint32_t)s.eyes;
            snprintf(fr.name, sizeof fr.name, "mock_rt"); snprintf(fr.eye1_name, sizeof fr.eye1_name, "mock_rt1");
            if (send(s.sock, &fr, sizeof fr, MSG_NOSIGNAL) != (ssize_t)sizeof fr) break;
            sent++;
            struct bmvr_xfer_ack ack;
            ssize_t n = recv(s.sock, &ack, sizeof ack, MSG_WAITALL);
            if (n != (ssize_t)sizeof ack) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { fprintf(stderr, "client: HANG: no ack for 5 s\n"); hang = 1; }
                break; /* the helper died or closed: the "game" itself carries on */
            }
            if (ack.magic != BMVR_XFER_MAGIC_ACK) break;
            acked++;
            struct timespec nap = { 0, (long)(1e9 / fps) };
            nanosleep(&nap, NULL);
        }
        session_close(&s);
        if (!reconnect) break;
    }
    printf("client: sent=%llu acked=%llu connects=%d last_tick=%u eyes=%d size=%ux%u hang=%d\n", (unsigned long long)sent, (unsigned long long)acked, connects, tick, eyes_seen, w_seen, h_seen, hang);
    return hang ? 3 : 0;
}
