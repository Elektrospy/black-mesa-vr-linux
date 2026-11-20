# Linux port: native OpenXR consumer (stage 4a)

Status: **milestone X0 (the probe) is implemented, compiled and run against Monado 21.0.0 in a CPU-only sandbox** (simulated
HMD, null compositor, Mesa lavapipe). Details and limits: "X0 results" below. **Not checked: SteamVR, any real GPU, any real
headset.** X1-X4 (frames, the pair ring, the pose channel) do not exist yet. Runtime-specific claims that X0 has not settled are
still marked *(verify)*.

Prerequisites: `docs/LINUX_PORT.md` (stages 2a-3d), `docs/LINUX_STEREO_HANDOFF.md` (protocol v4, pair atomicity),
`docs/openxr-eye-publish.md` (the Windows pipeline and the defects this design must not repeat).

Stage 3d state this builds on: the two-eye transport is verified in the real game (pairs, semaphores, kill loop 30/30 with
`--eyes 2`), and the author reports eye 1 carrying a real second target (`_rt_gblightdiff`). The snapshot numbers for that run
are not yet recorded in `LINUX_PORT.md`; record them there before relying on this paragraph.

## Goal and non-goals

Goal: the pair the game already delivers (two images, one atomic publication) reaches a headset through OpenXR, on **SteamVR
and Monado**, without CPU readback, with the same crash-safety properties as the transport (game unaffected by helper death,
helper reconnects, one helper per game folder).

Not in this stage:

- A real left/right picture. The engine still renders one view, so the content is mono until the game-hook port (plan item 4).
  This stage proves the display path, the pacing and the pose plumbing *interface*; it does not claim stereo gameplay.
- Input/controllers (OpenXR actions). The Windows action manifests under `VR/SteamVRActionManifest` are OpenVR; an OpenXR
  action layer is a separate stage.
- Hand, HUD and weapon-wheel rendering (plan item 5).

Acceptance (per `AGENTS.md`: "visible gameplay in the headset"): this stage ends when the game's picture is visible in a headset
through each runtime. It is **not** a stereo or gameplay-verified stage, and its status lines must say so.

## Decision 1: one process, a second sink in the helper

Options considered:

| Option | Verdict |
|---|---|
| A. Separate `bmvr_openxr_consumer` process fed by the helper | Rejected. It needs a second hand-off (images and semaphores re-exported), doubling the sync surface the 3a/3b work just made safe |
| B. Extend `bmvr_xfer_helper` with an OpenXR **sink** (`--sink xr`) | **Chosen** |

Why B: `XR_KHR_vulkan_enable2` makes the *runtime* choose the physical device and create the `VkInstance`/`VkDevice`
(`xrGetVulkanGraphicsDevice2KHR`, `xrCreateVulkanInstanceKHR`, `xrCreateVulkanDeviceKHR`). The helper's exportable images and
semaphores must live on that same device, or the two-process copy problem comes back. So in XR mode the helper's device creation
moves behind the runtime; everything else (socket, `flock`, hello/frame/ack, kill-loop behaviour) stays as it is.

Resulting structure (C11, 64-bit, same style as the helper):

```
linux/bmvr_xfer_helper.c      transport + --sink snapshot (today's behaviour, kept: it is what the tests use); --xr-probe entry
linux/bmvr_xr_sink.c  (new)   OpenXR instance/session/swapchain (X0: exists, probe only); frame loop thread + pair ring (X1+: not yet)
linux/bmvr_xr_sink.h  (new)   X0: bmvr_xr_probe(); the transport-facing interface below arrives with X2
third_party/openxr/           ALREADY vendored (Khronos headers 1.1.60, Apache-2.0 OR MIT); the loader is dlopen'd, like the Vulkan loader
```

Interface (transport -> sink), kept tiny on purpose:

```
sink_init(opts)                      -> picks/creates VkInstance, VkDevice, queue; returns them to the transport
sink_recommended_size(&w, &h)        -> used for the hello (see Decision 3)
sink_on_pair(slot_images, pose_id, tick)  -> "a complete pair is in your ring slot"
sink_shutdown()
```

`--sink snapshot` (default) keeps the current code path byte-for-byte, so T1-T19 and the kill loop are unaffected.
If `--sink xr` is requested and no OpenXR runtime or session is available the helper **exits with the reason**; it does not
silently fall back to snapshots (a silent fallback would look like a working headset path).

## Decision 2: the helper's XR thread is the pacing master

Windows lesson (`openxr-eye-publish.md`): the game ran ~200 fps against a 90 Hz compositor; most of the defects were about
two clocks sharing images. Design rules that carry over:

1. **A pair is never split** (already guaranteed by protocol v4). The sink stores a pair as one ring slot.
2. **The game never writes an image the compositor path may still read.** The game's shared images are *transfer
   buffers*, not display buffers: on each frame message the transport immediately submits a GPU copy
   `shared eye0/eye1 -> ring slot k` and signals `vk_done` (existing semantics: the ack means "queue submission done").
3. **The XR thread displays the newest complete slot.** After `xrWaitFrame` it picks the newest slot in state READY, copies it
   into the acquired swapchain images, releases, and `xrEndFrame`s. If no new pair arrived, it re-copies the previous slot
   (a swapchain image cannot be resubmitted after release); the compositor reprojects. This is Windows defect 3
   ("starved frames") handled by construction.
4. **Slot states under one mutex:** FREE -> WRITING (transport) -> READY -> READING (XR thread) -> FREE. Three slots:
   one being written, one READY, one being read. The transport writes into the oldest FREE slot, or overwrites the older READY
   slot when the game outpaces the display (newest wins; the dropped count is logged, rate-limited).
5. **No `WaitDeviceIdle`-style stalls and no log writes per frame.** Rate lines every 5 s, like the Windows rate log
   (`pairs=N/s displayed=N/s reused=N/s dropped=N/s`).

Threads: the existing socket thread; one new XR thread (event poll, frame loop). Vulkan queues need external synchronization and
the runtime may also use the queue it is given in the session binding *(verify the exact rule in the XR spec's Vulkan binding
section)*. First implementation: ONE queue, ONE mutex around submissions by both threads. Request a second queue only if the
rate log shows contention.

Cost: one extra GPU copy of both eyes per pair (~50 MB per eye at 3840x2160 RGBA8, a ~1 ms-class GPU copy on the author's RTX 3080
class hardware, *estimate, to be measured in X2*). In exchange the game's frame rate and the display rate are fully decoupled.

## Decision 3: sizes and formats (keep the helper a raw copy)

- The shim already scales on the GL side (`target=...|size:WxH`, hello carries the image size). So the sink asks the runtime
  for `recommendedImageRectWidth/Height` (`xrEnumerateViewConfigurationViews`, `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`) and
  offers images of exactly that size in the hello. The GL blit does the resampling; the Vulkan side does `vkCmdCopyImage`, no
  shader, no filtering.
- The shared images are `R8G8B8A8_UNORM` today. The engine's final colour is already gamma-encoded, so the bytes must be
  presented as sRGB: create the swapchain as `VK_FORMAT_R8G8B8A8_SRGB` and raw-copy the bytes (same channel order, same texel
  size, so the copy is legal and exact). If a runtime offers only UNORM or only BGRA formats, the picture would be washed out or
  channel-swapped; the sink logs `xrEnumerateSwapchainFormats` in full and **refuses** to start rather than guess. A shader
  pass is the documented fallback and is out of scope until a runtime actually needs it *(verify in X0 on both runtimes)*.
- Layer flags: do not set `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`; the engine's alpha channel is not meaningful
  for the final image.
- A mismatch between the images in the hello and the swapchain size is an error at session creation, not at frame time.

## Decision 4: what is submitted while the content is mono

Two layer modes, selectable (`--xr-layer quad|projection`):

- **quad** (default for X2): a head-locked or fixed-in-space `XrCompositionLayerQuad` showing eye 0. A flat screen in front of
  the user. Correct for 2D content, needs no pose from the game, and is the simplest thing that puts the game in the headset.
- **projection** (X3 onward): an `XrCompositionLayerProjection` with the pair as the two views, with **pose and FOV taken from
  the slot** (never from "now"). With mono content and no game pose, the slot's pose is a pinned one (identity in `LOCAL`
  space) and the picture is head-locked. This exists to prove eye order and the pair path, e.g. left = final image,
  right = `_rt_gblightdiff`, so a wrong eye order or a dropped eye is visible at once.

## Decision 5: pose flow (interface defined now, implemented with the hook port)

The Windows lesson (`openxr-eye-publish.md` §7a): the pose submitted with a frame must be the pose the frame was **rendered**
with, and it travels with the frame, not read at display time. On Linux the game needs the per-eye view/projection *before*
the engine renders, so a helper -> shim direction is required. Proposed **protocol v5** (not implemented; v4 peers keep working
only if the helper sees no pose-capable shim, otherwise reject with both versions logged, like T18):

```
pose   (helper -> shim, new, sent ahead of need, e.g. once per xrWaitFrame):
         pose_id u32, predicted_display_time i64,
         per eye: position xyz f32, orientation xyzw f32, fov left/right/up/down f32 (radians; XrFovf order)
frame  (shim -> helper): + pose_id u32   "this pair was rendered with pose pose_id"
```

- The sink keeps the last N poses by id; a READY slot records the `pose_id` it was rendered with and the layer uses exactly
  that pose and FOV. An unknown id (too old) marks the slot unusable for the projection layer and is counted.
- Fixed-width fields only, same `_Static_assert` rule as v4; position/orientation are OpenXR's own types (right-handed, +Y
  up, -Z forward): convert to Source's coordinate system on the **game side** (one conversion point, documented with the hooks).
- The engine's `CViewSetup` takes a symmetric FOV and an aspect; OpenXR FOVs are asymmetric per eye, so exact frustums need the
  off-centre projection override the Windows port used (`NormalizeViewSetupForVREye`, `openxr-eye-publish.md` defect 5: the
  submitted FOV must match the rendered frustum or the compositor reprojects through the wrong projection). The sink therefore
  never "improves" an FOV: it submits what the slot was rendered with.
- Depth layer (`XR_KHR_composition_layer_depth`): the Windows notes attribute walking judder to having no depth. `_rt_gbdepthrt`
  is a candidate source, but its encoding is not known to be linear or in the range the extension wants. Out of scope; record as
  an open question.

## Runtimes: SteamVR and Monado

The consumer uses the loader's active runtime (`XR_RUNTIME_JSON` overrides it) and logs `xrGetInstanceProperties` (runtime name,
version), the enabled extensions and the swapchain formats on every start. It does not special-case a runtime by name unless a
defect forces it, and every special case gets a documented reason (rule 3 in `AGENTS.md`).

| | SteamVR | Monado |
|---|---|---|
| Select | SteamVR must be the *active* OpenXR runtime, not just running. The Developer-tab button "Set SteamVR as OpenXR runtime" does not exist on every Linux install (missing on one tester's); symlinking `<SteamVR dir>/steamxr_linux64.json` to `~/.config/openxr/1/active_runtime.json` worked (**observed**), or set `XR_RUNTIME_JSON` | `XR_RUNTIME_JSON=<path to openxr_monado.json>` (distro-dependent path), run `monado-service` first *(verify)* |
| Headset-free testing | none; needs a headset (or a null driver, *verify*) | `SIMULATED_ENABLE=1` (simulated HMD) or `QWERTY_ENABLE=1`, with `XRT_COMPOSITOR_NULL=1`: **names present in Monado 21.0.0 and the simulated HMD verified in X0** |
| Vulkan binding | `XR_KHR_vulkan_enable2` expected *(verify)* | `XR_KHR_vulkan_enable2` offered: **verified in X0 (Monado 21.0.0)** |
| Likely differences to check | runtime-owned device choice; swapchain formats offered; behaviour when the session is not focused | compositor choice (direct mode vs. window); formats; frame timing on the null/sim path |

Process placement: only the helper talks to OpenXR; the game does not. So the helper needs the runtime's socket/JSON, the game
does not. If the helper runs on the host and the game in a container (as the Steam Linux Runtime does), the runtime lives on the
host side with the helper. If the helper ever runs *inside* the container, the runtime path must be exposed to it; this is the
only place the container matters.

Hybrid-GPU hazard: the runtime picks the physical device; the game's GL context may be on another GPU (PRIME). The current
transport already requires both to be the same GPU for the opaque-fd import to work. Add a startup check: compare the device UUID the
runtime chose with the one the shim's GL context reports (`GL_EXT_memory_object` device UUID) and fail with both UUIDs named,
instead of letting the import fail obscurely *(needs a small hello/handshake addition; track as part of X2)*.

## Milestones

Each milestone is runnable on its own; statuses use the `AGENTS.md` ladder (implemented / compiled / launched /
runtime-initialized / ... / headset presentation verified). A status is written down only when it was observed.

| | Deliverable | Needs | Pass criteria |
|---|---|---|---|
| X0 | `bmvr_xr_sink` skeleton, `--xr-probe`: instance, system, view config, swapchain formats, session create/destroy, no game, no frames | Monado **and** SteamVR | Logs runtime name/version/extensions/formats on both; confirms or corrects every *(verify)* above; helper exits cleanly. **Status: implemented; Monado 21.0.0 (sandbox) PASS; SteamVR not run** |
| X1 | `--selftest-xr`: frame loop with a generated pattern (left tinted one way, right the other, frame counter), quad then projection | Monado sim/qwerty first, then a headset | Correct eye order; steady `xrWaitFrame` cadence; rate lines sane; no per-frame disk writes |
| X2 | Socket sink: pair -> ring -> swapchain, quad layer, eye 0 | game + runtime | Game picture visible in the headset on each runtime; `test_kill_loop.sh -n 10`, then 30, passes with `--sink xr` (helper killed at random points: game unaffected, helper-less period shows nothing or last image, reconnect restores the picture) |
| X3 | Projection layer with a pinned pose; `target2=name:_rt_gblightdiff` | as X2 | Left and right visibly different content, correct sides, no eye ever shows a different tick than the other |
| X4 | Protocol v5 pose channel, mock-tested (`mock_game` replays poses; new case T20+) | none (stand-ins) | Pair slots carry their own `pose_id`; stale id handled; v4 peer rejected with both versions named. Real use waits for the hook port |

Test additions in `tests/linux`: a stand-in for the XR side is not practical (the point is the real runtime), so X1-X3 are
manual or runtime-assisted; X4 and the ring state machine (slot states, newest-wins, reuse-when-starved, no half pair) are unit
tested with a fake sink driver, in the same style as T14-T19. The ring logic is the part most likely to hide a race, so it gets
the most tests: concurrent writer/reader stress with a counter pattern checked for "both eyes carry the same tick".

## X0 results

`bmvr_xfer_helper --xr-probe [--xr-wait MS] [--xr-size WxH]` needs no game, no game folder and none of the helper's own Vulkan
setup. It prints every step with ok/FAIL/note, then a summary and a verdict: exit **0** PASS, **1** a required item missing or
failing, **2** UNAVAILABLE (no loader, no runtime, runtime installed but its service not running, or no headset system).
`tests/linux/test_xr_probe.sh [--software]` runs it against its own headless Monado (cases X0-a..e). It was written and run
in a sandbox with no GPU and no headset: **14/14 checks passed**, 10 probes in a row exited 0 (teardown is stable), and the
existing stand-in suite (T1-T19, host-native) still passes with the helper change.

Facts observed (Monado 21.0.0 from the Ubuntu 24.04 package, `SIMULATED_ENABLE=1 XRT_COMPOSITOR_NULL=1`, Mesa lavapipe
as the Vulkan device, 2026-11-20). They settle the corresponding *(verify)* marks **for Monado in this setup only**:

| Question from this design | Observed | Holds for SteamVR / real hardware? |
|---|---|---|
| Does the runtime offer `XR_KHR_vulkan_enable2`? | yes (also `XR_KHR_vulkan_enable`, `XR_KHR_composition_layer_depth`) | unknown for SteamVR |
| Does the runtime-owned device flow work (requirements -> `xrCreateVulkanInstanceKHR` -> `xrGetVulkanGraphicsDevice2KHR` -> `xrCreateVulkanDeviceKHR` -> `xrCreateSession`)? | yes; Vulkan 1.1 instance accepted (Monado reports its upper bound as 1023.1023 = no limit) | unknown |
| Is `R8G8B8A8_SRGB` offered, and does a swapchain in it with `TRANSFER_DST` usage work (the raw-copy assumption)? | yes: offered (also `R8G8B8A8_UNORM`, both BGRA, `R16G16B16A16_SFLOAT`), 3 images | **the biggest remaining unknown for SteamVR** |
| Can the helper's exportable images live on the runtime's device? | yes on lavapipe (opaque-fd image export reported exportable) | must be re-checked on the real GPU |
| Is the swapchain limited to the view's `maxImageRect`? | no: a 3840x2160 swapchain was created although the simulated HMD reports max 1024x1024 per view | runtime-specific; do not rely on it |
| Does the session reach `READY` without frames? | yes (`IDLE` -> `READY`) | unknown |
| What does "runtime installed, service not running" look like? | `xrEnumerateInstanceExtensionProperties` still works, `xrCreateInstance` fails with `XR_ERROR_RUNTIME_FAILURE` (-2): reported as UNAVAILABLE with a hint | SteamVR will differ |
| Monado quirk | `monado-service` exits at startup if its stdin cannot be polled (`/dev/null`): the test gives it an open FIFO | n/a |

Not observable in this setup (and therefore still open): opaque-fd **semaphores** (lavapipe has no
`VK_KHR_external_semaphore_fd`, the probe reports it as a note, so `sync=sem` is untested here), the real recommended view
size (the simulated HMD reports 128x128), the device UUID comparison with the game's GL context (printed, not compared yet),
the queue-sharing rule, and everything about frames.

## Risks and open questions

1. **Queue synchronization with the runtime** (one queue, two threads, runtime submits on it too): exact rule to confirm in X0,
   and the mutex scope to measure in X2.
2. **Swapchain format availability**: holds on Monado 21.0.0 (X0, sandbox); SteamVR still unchecked, and it is the biggest
   single "might not hold".
3. **Latency with mono + pinned pose**: irrelevant to a head-locked quad, but it means this stage cannot say anything about
   motion-to-photon. That is only measurable once poses flow (X4 + hook port).
4. **SteamVR on Linux session lifecycle** (session state READY/FOCUSED transitions, losing focus while the game window has it):
   needs a headset run to know; the XR thread must treat `XR_SESSION_STATE_*` as a state machine, not assume FOCUSED.
5. **Helper death while the session is running**: the compositor keeps the last submitted layer or drops it, runtime-dependent
   *(verify)*; the XR thread should keep submitting frames from the last READY slot while the game connection is down, so a
   reconnect does not look like a session restart.
6. **Extension/ABI drift of the OpenXR loader on the user's distro**: dlopen `libopenxr_loader.so.1` and call
   `xrGetInstanceProcAddr` for everything, as the helper does for Vulkan; log and exit if the loader is missing.
7. **Eye-count assumptions**: `--eyes 2` must be required for `--sink xr` projection mode; quad mode works with `--eyes 1`.

## Order of work

1. ~~Record the `_rt_gblightdiff` result in `LINUX_PORT.md`~~ (recorded as reported by the author; the snapshot numbers are still to be added).
2. X0: Monado done (above). **Next: run `bmvr_xfer_helper --xr-probe` with SteamVR on a real machine** and fold the output into
   "X0 results" (a second column); a failed required item there changes the design before X1 is written.
3. X1, X2, X3 in order; X4 in parallel with the start of the hook port.
4. Only then is `docs/LINUX_PORT.md`'s plan item 3 ("hand both to a native OpenXR helper") marked done.
