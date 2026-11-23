# OpenXR sink in the helper: design (stage 4a)

Goal: the pair the game already delivers reaches a headset through OpenXR on SteamVR and Monado, without CPU readback, with the
transport's crash-safety (game unaffected by helper death, helper reconnects, one helper per game folder). Results of each
milestone are in [openxr-results.md](openxr-results.md).

Acceptance follows `AGENTS.md`: visible gameplay in the headset. The OpenXR stage on its own proves the display path, pacing and
pose plumbing, not gameplay.

## Decision 1: one process, a second sink in the helper

| Option | Verdict |
|---|---|
| A. Separate `bmvr_openxr_consumer` process fed by the helper | rejected: a second hand-off (images and semaphores re-exported) doubles the sync surface |
| B. `bmvr_xfer_helper` gets an OpenXR **sink** (`--sink xr`) | **chosen** |

`XR_KHR_vulkan_enable2` makes the *runtime* choose the physical device and create the `VkInstance`/`VkDevice`
(`xrGetVulkanGraphicsDevice2KHR`, `xrCreateVulkanInstanceKHR`, `xrCreateVulkanDeviceKHR`). The helper's exportable images and
semaphores must live on that device, so in XR mode device creation moves behind the runtime. Socket, `flock`, hello/frame/ack and
kill-loop behaviour stay as they are. `--sink snapshot` (default) keeps the old path byte for byte. If `--sink xr` is requested and
no runtime or session is available the helper exits with the reason; a silent fallback would look like a working headset path.

Files: `bmvr_xfer_helper.c` (transport, snapshot sink, `--xr-probe` entry), `bmvr_xr_sink.c/.h` (OpenXR instance/session/swapchain,
probe, self-test, socket sink, XR thread, input, finger tracking), `bmvr_pair_ring.h`, `bmvr_pose_store.h`. OpenXR headers are
vendored in `third_party/openxr` (Khronos 1.1.60); the loader is `dlopen`ed like the Vulkan loader.

## Decision 2: the XR thread is the pacing master

The Windows lesson (`docs/openxr-eye-publish.md`): the game ran ~200 fps against a 90 Hz compositor and most defects were about two
clocks sharing images.

1. A pair is never split; the sink stores it as one ring slot.
2. The game never writes an image the compositor path may still read. The shared images are *transfer buffers*: on each frame
   message the transport submits a GPU copy `shared eye0/eye1 -> ring slot k` and signals `vk_done`.
3. After `xrWaitFrame` the XR thread picks the newest complete slot, copies it into the acquired swapchain image, releases and
   `xrEndFrame`s. If no new pair arrived it re-copies the previous slot (the compositor reprojects).
4. Slot states under one mutex: FREE -> WRITING -> SUBMITTED -> READY -> READING -> FREE. Three slots. The writer takes the oldest
   FREE slot or overwrites the older READY slot (newest wins, drops counted). The slot being displayed is never recycled.
5. No `WaitDeviceIdle`-style stalls and no per-frame log writes; rate lines every 5 s
   (`pairs=N/s displayed=N/s reused=N/s dropped=N/s`).

Cost: one extra GPU copy of both eyes per pair (~50 MB per eye at 3840x2160 RGBA8; copy time is still an estimate, only the round
trip is measured).

**Amendment (2026-11-21, real game): one queue was not enough.** With one `VkQueue` and one mutex on SteamVR 2.19.4 + RTX 3080 the
transport waited 4 ms on average (10 ms max) for the lock at 30 pairs/s, and 18-21 ms (90-97 ms max) at `sync=finish`, `every=1`,
with the XR frame rate collapsing from 90/s to about 5/s. The validation layer reported `UNASSIGNED-Threading-MultipleThreads-Write`
and the kernel logged NVIDIA Xid 32 and Xid 69 against `bmvr_xfer_helper` in three sessions, each followed by SteamVR's compositor
watchdog (`WaitForPresent` hung 6.8 s). Which thread was the second submitter is not proven (the runtime lives in the helper process
and may submit on the session's queue from XR calls outside the mutex).

**Fix:** the runtime-created device is requested with two queues of the graphics family. Queue 0 stays the session's queue (XR thread and
runtime); queue 1 belongs to the transport thread alone, its submits take no mutex. `qmu` is used only when the family has fewer than
two queues or `BMVR_XR_ONE_QUEUE=1`. The helper logs `queues: transport thread on its own queue (family N has M)`; **if that line is
missing the installed helper is old.** No extra synchronisation is needed between the queues: the XR thread only picks a slot whose
copy fence has signalled, all images are `VK_SHARING_MODE_EXCLUSIVE` in one family, the ring's mutex orders slot states. After the fix:
no threading error, lock wait 0.000 ms, 89-90 XR frames/s.

## Decision 3: sizes and formats (the helper stays a raw copy)

- The sink asks the runtime for `recommendedImageRectWidth/Height` and offers images of that size in the hello; the shim's GL blit
  does the resampling, the Vulkan side does `vkCmdCopyImage` (no shader, no filtering).
- Shared images are `R8G8B8A8_UNORM`. The engine's colour is already gamma-encoded, so the swapchain is `R8G8B8A8_SRGB` and the bytes
  are raw-copied. If a runtime offers only UNORM or only BGRA the sink logs all formats and refuses to start rather than guess.
- No `BLEND_TEXTURE_SOURCE_ALPHA`: the engine's alpha is not meaningful.
- A size mismatch is an error at session creation, not at frame time.

## Decision 4: layers

- **quad** (`--xr-layer quad`): a flat screen showing eye 0, 2 m ahead, 1.6 m high; `--xr-quad world` (default; fixed in the room, LOCAL
  space) or `head` (VIEW space).
- **projection** (`--xr-layer projection`): `XrCompositionLayerProjection` with the two views, pose and FOV taken from the slot, never
  from "now". Needs the pose channel. With `pose_id` 0 (the game did not draw with a pose) the layer is head-locked.

The launcher uses `projection`.

## Decision 5: pose flow (protocol v5, implemented)

The submitted pose is the pose the frame was rendered with and it travels with the frame. The XR thread locates both eyes in LOCAL
space every `shouldRender` frame (`xrLocateViews`, predicted display time) and keeps them in the pose table (`bmvr_pose_store.h`: last
32 poses by id, mutex, stale counter). The transport reads the newest for each ack and records each pair's `pose_id` in
`slot_pose[slot]` before `bmvr_ring_end_write`. A pair with `pose_id` N is displayed with exactly pose N's eye poses and FOVs; an id
no longer stored means no layer that frame, counted as `stale=`. The sink never "improves" an FOV: OpenXR FOVs are asymmetric per
eye and the game draws them through the off-centre setup ([stereo-views.md](stereo-views.md)).

Depth layer (`XR_KHR_composition_layer_depth`): the Windows notes attribute walking judder to having no depth. `_rt_gbdepthrt` is a
candidate source but its encoding is not known to be in the range the extension wants. Out of scope; open.

## Risks and open questions

1. ~~Queue synchronisation with the runtime~~ resolved by the second queue; which XR call submits on the session's queue is unknown.
2. Swapchain formats: sRGB raw copy is the only RGBA8 route on SteamVR 2.19.4 (UNORM not offered). Other versions / GPUs unchecked.
3. Latency with a pinned pose is meaningless; motion-to-photon needs the real pose path and has not been measured.
4. Session lifecycle: the XR thread treats `XR_SESSION_STATE_*` as a state machine. Observed in the real game: states alternate when
   the headset is taken off and put on; the loop keeps running.
5. Helper death while the session runs: the compositor keeps or drops the last layer (runtime-dependent). Observed in stand-ins: the
   helper keeps showing the last pair while the game is away and a reconnect restores the live picture.
6. Loader/ABI drift: `libopenxr_loader.so.1` is `dlopen`ed and everything goes through `xrGetInstanceProcAddr`; a missing loader is
   logged and the helper exits.
7. `--eyes 2` is required for projection mode; quad works with `--eyes 1`.
8. Hybrid GPU: the runtime picks the physical device, the game's GL context may be on another. The helper prints the runtime's device
   UUID; the automatic comparison with the shim's GL UUID is **not implemented** (needs a hello addition).
9. Process placement: only the helper talks to OpenXR. If the helper ever ran inside the Steam container the runtime path would need
   to be exposed to it.
