/*
 * Black Mesa VR - native Linux port, stage 4a: OpenXR sink for the 64-bit helper.
 * Design: docs/LINUX_OPENXR_CONSUMER.md. This file is the interface the helper (bmvr_xfer_helper.c) uses.
 *
 * Milestone X0 (this file's current content): --xr-probe. It opens the OpenXR runtime the way the real sink will
 * (loader -> instance -> system -> runtime-chosen Vulkan device -> session -> swapchain) but submits no frames, and
 * prints what it found, so that the assumptions in the design document become observed facts.
 *
 * Later milestones add the pair ring and the frame loop here (X1-X3); nothing of X1+ exists yet.
 */
#ifndef BMVR_XR_SINK_H
#define BMVR_XR_SINK_H

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

#ifdef __cplusplus
}
#endif
#endif
