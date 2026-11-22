/*
 * Black Mesa VR - native Linux port, stages 2a/2b/3a/3d: GPU hand-off.
 * Wire protocol between the 32-bit shim (bmvr_preload.c, inside the game) and the
 * 64-bit helper (bmvr_xfer_helper.c). Included by both: fixed-width types only, so the
 * structs have the same layout in a 32-bit and a 64-bit build (checked below).
 *
 * Transport: a unix stream socket "<game folder>/bmvr_xfer.sock" created by the helper.
 *
 *   helper -> shim   struct bmvr_xfer_hello + ONE file descriptor (SCM_RIGHTS): the exported
 *                    Vulkan device memory of the shared image (three fds with semaphores, v3)
 *   shim   -> helper struct bmvr_xfer_frame   "the shared image now holds a frame"
 *   helper -> shim   struct bmvr_xfer_ack     "I read it" (the shim waits for this, so a
 *                    frame is never overwritten while the helper reads it)
 *
 * Default sync (stages 2a/2b, verified): the shim does a full glFinish and waits for the helper's
 * ack, so a frame is never overwritten while the helper reads it. Stage 3a adds GPU semaphores
 * (below) as an opt-in that falls back to this when anything is missing.
 *
 * Version 5 (stage 4a, X4): POSES. The helper (OpenXR sink) tells the game where the headset's eyes are, and the game says which
 * pose each pair was rendered with. There is no separate helper->shim message: every ack carries the helper's NEWEST pose (struct
 * bmvr_xfer_pose, pose_id 0 = none), so the shim never has to poll the socket and the exchange stays strictly request/response. The shim
 * keeps the pose of the ack it last saw, uses that one for its next frame, and stamps frame.pose_id with it. The sink keeps the last 32
 * poses by id and submits a pair with exactly the pose/FOV it was rendered with. Offered by the hello with BMVR_XFER_CAP_POSES; a helper
 * that does not offer it sends pose_id 0 and the shim sends pose_id 0. Poses are OpenXR's own convention (right-handed, +Y up, -Z
 * forward); converting to Source space is the game side's job (one place, with the hook port).
 *
 * Version 4 (stage 3d): TWO EYES. A pair is the unit of publication (never one eye at a time, so the two eyes of
 * a pair always come from the same game frame). The hello says how many images the helper offers (eye_count, 1 or 2)
 * and carries one memory fd per image, then the two semaphore fds. The frame message describes both eyes; ONE
 * semaphore pair covers the pair:
 *     GL:     wait(vk_done) [not before the first pair] -> blit eye 0 -> blit eye 1 -> signal(gl_ready) -> flush
 *     helper: wait(gl_ready) -> read both images -> signal(vk_done)
 * The shim uses eye 1 only when it was asked to (target2 in bmvr_target.txt) AND the helper offers two images; a
 * frame with eye_count == 1 on a two-image session only touches image 0. Design: docs/LINUX_STEREO_HANDOFF.md.
 *
 * Version 3 (stage 3a): GPU semaphores. When the helper offers them (hello.caps has
 * BMVR_XFER_CAP_SEMAPHORES) the hello carries THREE fds: the image memory, then two
 * VK_KHR_external_semaphore_fd opaque-fd semaphores. The shim imports them as GL semaphores
 * (GL_EXT_semaphore_fd) and, per frame, orders the two APIs on the GPU instead of with a CPU stall:
 *     GL:     wait(vk_done) [not before the first frame] -> blit -> signal(gl_ready) -> flush
 *     helper: wait(gl_ready) -> copy the image -> signal(vk_done)
 * The frame message then carries BMVR_XFER_FLAG_SEMAPHORE and the ack means "queue submission
 * done" (the vk_done signal is guaranteed to be pending), not "readback finished".
 *
 * Version 2 (stage 2b): the frame message says WHAT was blitted (an engine render target, by
 * label, or the window) and whether the blit flipped Y, so the helper can name its snapshots
 * and the person reading them can tell which orientation is the upright one.
 */
#ifndef BMVR_XFER_H
#define BMVR_XFER_H

#include <stddef.h>
#include <stdint.h>

#define BMVR_XFER_SOCKET_NAME "bmvr_xfer.sock"
#define BMVR_XFER_VERSION 6u
#define BMVR_XFER_MAGIC_HELLO 0x31564d42u /* "BMV1" */
#define BMVR_XFER_MAGIC_FRAME 0x31524642u /* "BFR1" */
#define BMVR_XFER_MAGIC_ACK   0x314b4341u /* "ACK1" */

#define BMVR_XFER_CAP_SEMAPHORES 1u /* hello carries 3 fds: memory, gl_ready semaphore, vk_done semaphore */
#define BMVR_XFER_CAP_POSES      2u /* v5: every ack may carry a pose (the helper submits a projection layer with game-rendered poses) */
#define BMVR_XFER_CAP_INPUT      4u /* v6: every ack carries the controllers' state (struct bmvr_xfer_input); seq 0 = none yet */

struct bmvr_xfer_hello {
    uint32_t magic;             /* BMVR_XFER_MAGIC_HELLO */
    uint32_t version;           /* BMVR_XFER_VERSION */
    uint32_t width, height;     /* size of the shared image */
    uint32_t gl_internal_format;/* GL enum for the texture, 0x8058 = GL_RGBA8 (== VK_FORMAT_R8G8B8A8_UNORM) */
    uint32_t dedicated;         /* 1: memory was allocated as a dedicated allocation */
    uint32_t optimal_tiling;    /* 1: image uses VK_IMAGE_TILING_OPTIMAL */
    uint32_t caps;              /* BMVR_XFER_CAP_* */
    uint32_t eye_count;         /* v4: shared images offered, 1 or 2 (one memory fd each, same size/format); then the semaphore fds */
    uint32_t reserved;          /* keeps memory_size at offset 40 in both 32- and 64-bit builds */
    uint64_t memory_size;       /* VkMemoryAllocateInfo::allocationSize (of each image) */
};

#define BMVR_XFER_FLAG_FLIPPED 1u  /* the blit inverted Y (GL bottom-up -> top-down) */
#define BMVR_XFER_FLAG_WINDOW  2u  /* source was the window's default framebuffer, not a render target */
#define BMVR_XFER_FLAG_SEMAPHORE 4u /* the shim signalled gl_ready for this frame: wait on it, signal vk_done after reading */

struct bmvr_xfer_frame {
    uint32_t magic;             /* BMVR_XFER_MAGIC_FRAME */
    uint32_t tick;              /* shim-side counter, for the log / file names */
    uint32_t src_w, src_h;      /* size of the source frame that was blitted */
    uint32_t flags;             /* BMVR_XFER_FLAG_* */
    uint32_t gl_name;           /* GL texture/renderbuffer name of the source (0 for the window); diagnostics only */
    char     name[24];          /* engine render-target label, NUL-terminated ("window" for the window) */
    /* v4: the second eye of the pair. Valid only when eye_count == 2; eye 0 is described by the fields above, and
     * BMVR_XFER_FLAG_SEMAPHORE in `flags` applies to the whole pair. */
    uint32_t eye_count;         /* 1 or 2: images written for this frame (image 0 first) */
    uint32_t eye1_src_w, eye1_src_h;
    uint32_t eye1_flags;        /* BMVR_XFER_FLAG_FLIPPED / _WINDOW (SEMAPHORE is taken from `flags`) */
    uint32_t eye1_gl_name;
    char     eye1_name[24];
    /* v5: the pose (bmvr_xfer_ack.pose.pose_id) this pair was rendered with; 0 = none / the helper offers no poses. */
    uint32_t pose_id;
};

/* v5: where one eye is, in OpenXR's convention (right-handed, +Y up, -Z forward), relative to the pose's reference space. */
struct bmvr_xfer_eye_pose {
    float pos[3];               /* meters */
    float rot[4];               /* orientation quaternion x, y, z, w */
    float fov[4];               /* radians, XrFovf order: angleLeft, angleRight, angleUp, angleDown (left and down are negative) */
};

#define BMVR_XFER_POSE_LOCAL 1u /* the poses are in the runtime's LOCAL space (fixed in the room); otherwise VIEW (head-locked) */

struct bmvr_xfer_pose {
    uint32_t pose_id;           /* 0 = no pose; otherwise issued by the helper, increasing, wraps skipping 0 */
    uint32_t flags;             /* BMVR_XFER_POSE_* */
    int64_t  display_time;      /* XrTime the pose was predicted for (diagnostics; the compositor is given the frame's own time) */
    struct bmvr_xfer_eye_pose eye[2];   /* 0 = left, 1 = right */
};

/* v6: controllers. Fixed-width, 4-byte aligned fields only (identical in the 32-bit shim and the 64-bit helper). Poses are OpenXR's convention
 * (right-handed, +Y up, -Z forward, metres) in the same LOCAL space as the eye poses: pos xyz, then rotation quaternion xyzw. hand 0 = left, 1 = right. */
#define BMVR_XFER_INPUT_POSE_L   1u   /* flags: hand 0's aim/grip poses are valid */
#define BMVR_XFER_INPUT_POSE_R   2u   /* hand 1's */
#define BMVR_XFER_INPUT_FOCUSED  4u   /* the session has input focus (otherwise all values are zero) */
#define BMVR_XFER_BTN_PRIMARY    1u   /* per hand, shifted by 8 * hand: right a / left x */
#define BMVR_XFER_BTN_SECONDARY  2u   /* right b / left y */
#define BMVR_XFER_BTN_MENU       4u   /* right menu / left view */
#define BMVR_XFER_BTN_STICK      8u   /* thumbstick click */
#define BMVR_XFER_BTN_BUMPER     16u
struct bmvr_xfer_input {
    uint32_t seq;               /* 0 = no state yet; otherwise increases with every helper poll */
    uint32_t flags;             /* BMVR_XFER_INPUT_* */
    uint32_t buttons;           /* BMVR_XFER_BTN_* << (8 * hand) */
    float stick[2][2];          /* [hand][x, y], -1..1, +y up */
    float trigger[2], squeeze[2];
    float aim[2][7];            /* [hand] pos xyz, quat xyzw */
    float grip[2][7];
    uint32_t reserved;          /* pads the struct to 160 bytes so struct bmvr_xfer_ack is 272 on both 32- and 64-bit (int64 alignment inside the pose) */
};

struct bmvr_xfer_ack {
    uint32_t magic;             /* BMVR_XFER_MAGIC_ACK */
    uint32_t status;            /* 0 = ok. With BMVR_XFER_FLAG_SEMAPHORE: the helper's queue submission is done */
    struct bmvr_xfer_pose pose; /* v5: the newest pose (pose_id 0 = none yet / not offered) */
    struct bmvr_xfer_input input; /* v6: the controllers (seq 0 = none / not offered) */
};

_Static_assert(sizeof(struct bmvr_xfer_hello) == 48, "bmvr_xfer_hello layout differs between 32/64-bit");
_Static_assert(sizeof(struct bmvr_xfer_frame) == 96, "bmvr_xfer_frame layout");
_Static_assert(sizeof(struct bmvr_xfer_eye_pose) == 44, "bmvr_xfer_eye_pose layout");
_Static_assert(sizeof(struct bmvr_xfer_pose) == 104, "bmvr_xfer_pose layout differs between 32/64-bit");
_Static_assert(sizeof(struct bmvr_xfer_input) == 160, "bmvr_xfer_input layout");
_Static_assert(sizeof(struct bmvr_xfer_ack) == 112 + 160, "bmvr_xfer_ack layout");
_Static_assert(offsetof(struct bmvr_xfer_pose, display_time) == 8 && offsetof(struct bmvr_xfer_pose, eye) == 16 && offsetof(struct bmvr_xfer_ack, pose) == 8,
               "pose offsets differ between 32/64-bit (int64 alignment)");

#endif
