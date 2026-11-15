/*
 * Black Mesa VR - native Linux port, stage 2a: GPU hand-off test.
 * Wire protocol between the 32-bit shim (bmvr_preload.c, inside the game) and the
 * 64-bit helper (bmvr_xfer_helper.c). Included by both: fixed-width types only, so the
 * structs have the same layout in a 32-bit and a 64-bit build (checked below).
 *
 * Transport: a unix stream socket "<game folder>/bmvr_xfer.sock" created by the helper.
 *
 *   helper -> shim   struct bmvr_xfer_hello + ONE file descriptor (SCM_RIGHTS): the exported
 *                    Vulkan device memory of the shared image
 *   shim   -> helper struct bmvr_xfer_frame   "the shared image now holds a frame"
 *   helper -> shim   struct bmvr_xfer_ack     "I read it" (the shim waits for this, so a
 *                    frame is never overwritten while the helper reads it)
 *
 * The explicit ack replaces GL/Vulkan semaphores for this test. The real renderer will use
 * GL_EXT_semaphore_fd / VK_KHR_external_semaphore_fd instead (planned, not implemented).
 */
#ifndef BMVR_XFER_H
#define BMVR_XFER_H

#include <stdint.h>

#define BMVR_XFER_SOCKET_NAME "bmvr_xfer.sock"
#define BMVR_XFER_VERSION 1u
#define BMVR_XFER_MAGIC_HELLO 0x31564d42u /* "BMV1" */
#define BMVR_XFER_MAGIC_FRAME 0x31524642u /* "BFR1" */
#define BMVR_XFER_MAGIC_ACK   0x314b4341u /* "ACK1" */

struct bmvr_xfer_hello {
    uint32_t magic;             /* BMVR_XFER_MAGIC_HELLO */
    uint32_t version;           /* BMVR_XFER_VERSION */
    uint32_t width, height;     /* size of the shared image */
    uint32_t gl_internal_format;/* GL enum for the texture, 0x8058 = GL_RGBA8 (== VK_FORMAT_R8G8B8A8_UNORM) */
    uint32_t dedicated;         /* 1: memory was allocated as a dedicated allocation */
    uint32_t optimal_tiling;    /* 1: image uses VK_IMAGE_TILING_OPTIMAL */
    uint32_t reserved;
    uint64_t memory_size;       /* VkMemoryAllocateInfo::allocationSize */
};

struct bmvr_xfer_frame {
    uint32_t magic;             /* BMVR_XFER_MAGIC_FRAME */
    uint32_t tick;              /* shim-side counter, for the log / file names */
    uint32_t src_w, src_h;      /* size of the source frame that was blitted */
};

struct bmvr_xfer_ack {
    uint32_t magic;             /* BMVR_XFER_MAGIC_ACK */
    uint32_t status;            /* 0 = ok */
};

_Static_assert(sizeof(struct bmvr_xfer_hello) == 40, "bmvr_xfer_hello layout differs between 32/64-bit");
_Static_assert(sizeof(struct bmvr_xfer_frame) == 16, "bmvr_xfer_frame layout");
_Static_assert(sizeof(struct bmvr_xfer_ack) == 8, "bmvr_xfer_ack layout");

#endif
