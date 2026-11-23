/*
 * Black Mesa VR - native Linux port, stage 4a: OpenXR sink for the 64-bit helper.
 * Design: docs/linux/openxr-helper.md. This file is the interface the helper (bmvr_xfer_helper.c) uses.
 *
 * Milestone X0 (this file's current content): --xr-probe. It opens the OpenXR runtime the way the real sink will
 * (loader -> instance -> system -> runtime-chosen Vulkan device -> session -> swapchain) but submits no frames, and
 * prints what it found, so that the assumptions in the design document become observed facts.
 *
 * Milestone X1: --selftest-xr. The same session, then a frame loop with a generated pattern (left blue 'L', right red 'R',
 * a frame counter), as a quad and/or a stereo projection layer. No game involved.
 *
 * Milestone X2: the socket sink. The helper's transport (bmvr_xfer_helper.c, --sink xr) hands every pair the game delivers to
 * this sink: the sink owns the runtime-chosen Vulkan device, the shared images and semaphores the game imports, a 3-slot
 * pair ring (bmvr_pair_ring.h) and one XR thread that displays the newest complete pair on a head-locked quad (eye 0).
 *
 * The pose channel (X4) does not exist yet. X3 (projection layer, `layer = 1`) exists: both eyes, head-locked, runtime eye poses.
 */
#ifndef BMVR_XR_SINK_H
#define BMVR_XR_SINK_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include "bmvr_xfer.h"

#ifdef __cplusplus
extern "C" {
#endif

struct bmvr_xr_probe_opts {
    int wait_session_ms; /* how long to wait for XR_SESSION_STATE_READY after xrCreateSession (<= 0: do not wait) */
    int swapchain_w;     /* swapchain probe size; 0 = the runtime's recommended size */
    int swapchain_h;
};

/* Exit codes of bmvr_xr_probe() (also the process exit code of `bmvr_xfer_helper --xr-probe`). */
#define BMVR_XR_PROBE_OK          0 /* everything the sink needs is present */
#define BMVR_XR_PROBE_FAILED      1 /* the runtime is there but something the sink needs is missing / failed */
#define BMVR_XR_PROBE_UNAVAILABLE 2 /* no OpenXR loader, no runtime, or no headset system: nothing to test against */

int bmvr_xr_probe(const struct bmvr_xr_probe_opts *opts);

struct bmvr_xr_selftest_opts {
    int seconds;       /* per layer mode (<= 0: 10) */
    int layer_mask;    /* 1 = quad, 2 = projection, 3 = both (quad first) */
    int wait_ready_ms; /* how long to wait for the session to reach READY (<= 0: 10000) */
    int width;         /* eye image size; 0 = the runtime's recommended size */
    int height;
};

/* Exit codes as for the probe: 0 PASS (frames were submitted), 1 FAILED, 2 UNAVAILABLE (nothing to test against). */
int bmvr_xr_selftest(const struct bmvr_xr_selftest_opts *opts);

/* ------------------------------------------------------------------------------------------------ X2: socket sink */
struct bmvr_xr_sink; /* opaque */

struct bmvr_xr_sink_opts {
    int eyes;                                  /* images per pair the game may deliver: 1 or 2 */
    int no_semaphores;                         /* do not offer GPU semaphores (the game falls back to glFinish + ack) */
    int wait_ready_ms;                         /* how long to wait for the session to reach READY (<= 0: 10000) */
    int width, height;                         /* eye image size; 0 = the runtime's recommended size */
    int check_pairs;                           /* --xr-check N: read back and log a few pixels of the first N fresh pairs (debug; stalls ~1 frame each) */
    int quad_head_locked;                      /* 1: the quad follows the head (X2 default of the first version); 0: fixed in the room (LOCAL space), 2 m ahead of where the head was at session start */
    int layer;                                 /* 0 = quad (eye 0, X2), 1 = stereo projection layer (X3; needs eyes == 2, two swapchains, head-locked) */
    const volatile sig_atomic_t *abort_flag;   /* optional: set by the caller's signal handler; ends the wait for READY */
};

struct bmvr_xr_sink_info {
    uint32_t width, height;   /* the size of every shared image: offer exactly this in the hello */
    uint64_t memory_size;     /* VkMemoryAllocateInfo::allocationSize of each shared image (the hello's memory_size) */
    int eyes;
    int semaphores;           /* 1: the hello may carry BMVR_XFER_CAP_SEMAPHORES */
    int poses;                /* 1: the hello may carry BMVR_XFER_CAP_POSES (projection layer + LOCAL space: X4) */
    int input;                /* 1: the hello may carry BMVR_XFER_CAP_INPUT (controller actions were created: v6) */
};

/* Opens the runtime (loader, instance, runtime-chosen device, session, one swapchain), creates the shared images, the ring and the
 * GPU objects, waits for READY and starts the XR thread. Returns BMVR_XR_PROBE_OK / _FAILED / _UNAVAILABLE (exit codes as the
 * probe); on anything but OK nothing is left running and *out is NULL. The reason has been printed. */
int bmvr_xr_sink_open(const struct bmvr_xr_sink_opts *opts, struct bmvr_xr_sink **out);
void bmvr_xr_sink_info(const struct bmvr_xr_sink *k, struct bmvr_xr_sink_info *info);

/* A game connected. Fills fds[] with fresh exported fds (eyes memory fds, then gl_ready and vk_done when semaphores are offered;
 * the caller closes them after sending) and sets *nfds. Fresh semaphores per client. Returns 0, or -1 (the sink is unusable). */
int bmvr_xr_sink_client_begin(struct bmvr_xr_sink *k, int fds[4], int *nfds);

/* One complete pair is in the shared images (rendered with pose `pose_id`, 0 = none). Submits the GPU copy into a ring slot (wait gl_ready / signal vk_done when use_sem)
 * and returns right after vkQueueSubmit, which is what the ack promises. Without use_sem it returns after the copy finished.
 * Returns 0, or -1 on a fatal Vulkan failure (the caller drops the game and exits with the reason). */
int bmvr_xr_sink_pair(struct bmvr_xr_sink *k, uint32_t tick, int eyes, int use_sem, uint32_t pose_id, uint32_t src_w, uint32_t src_h);

/* X4: the newest pose the XR thread located (for the ack). 1 and *out filled, 0 when none yet or poses are not offered. */
int bmvr_xr_sink_latest_pose(struct bmvr_xr_sink *k, struct bmvr_xfer_pose *out);

/* v6: the newest controller state (for the ack). 1 and *out filled, 0 when input is not offered or nothing was polled yet. */
int bmvr_xr_sink_latest_input(struct bmvr_xr_sink *k, struct bmvr_xfer_input *out);

/* The game went away: wait (bounded) for our submissions to finish. The XR thread keeps showing the last pair. */
void bmvr_xr_sink_client_end(struct bmvr_xr_sink *k);

/* 0 once the XR thread has ended (the runtime ended the session, or a failure); *why then says why. */
int bmvr_xr_sink_alive(const struct bmvr_xr_sink *k, char *why, size_t why_size);

void bmvr_xr_sink_close(struct bmvr_xr_sink *k);

#ifdef __cplusplus
}
#endif
#endif
