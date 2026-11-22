# Linux port: native OpenXR consumer (stage 4a)

Status: **milestone X0 (the probe) is implemented, compiled, and PASSES on two runtimes**: Monado 21.0.0 in a CPU-only sandbox
(simulated HMD, null compositor, Mesa lavapipe) and SteamVR/OpenXR 2.19.4 on a real machine (NVIDIA RTX 3080, reported by the
author, one run). Details and limits: "X0 results" below. **Milestone X1 (`--selftest-xr`, the frame loop with a generated
pattern) is implemented, compiled (clean with `-Wall -Wextra`, also under `gcc -fanalyzer`) and PASSES on SteamVR 2.19.4 + RTX 3080
(author's run: 895 quad and 898 projection frames submitted, about 90 fps; the author reports seeing a blue "L" and a red "R" in
the headset)**; see "X1 results". The Monado X1 script has since been executed (11/11 on Monado 21.0.0 + lavapipe).
**Milestone X2 (`--sink xr`: socket sink, pair ring, XR thread, quad or projection layer) is COMPLETE (2026-11-21).** It passes its
stand-in tests on Monado + lavapipe (16/16, kill loop 30/30, ASan/UBSan and TSan clean) and, as reported by the author, was run and
fully tested with the real game on SteamVR/OpenXR 2.19.4 + RTX 3080 (NVIDIA open kernel module 620.73.04), `sync=sem`, two eyes; the
log-backed numbers are in "X2 results (real game, SteamVR)". Getting there found and fixed a queue-sharing defect: **one queue + one
mutex was not enough, the transport thread now has its own `VkQueue`** (Decision 2, amended). X3 and X4 (projection layer, pose
channel) are implemented and ran with the real game, but **X3's pass criterion is not met: the engine still renders one view**, so
both eyes carry the same flat picture (see the X3/X4 rows). Runtime-specific claims that X0 has not settled are still marked *(verify)*.

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
linux/bmvr_xr_sink.c  (new)   OpenXR instance/session/swapchain (X0: probe); --selftest-xr frame loop (X1); the socket sink: shared images, ring copies, XR thread (X2)
linux/bmvr_xr_sink.h  (new)   bmvr_xr_probe(), bmvr_xr_selftest(), and the transport-facing interface of X2 (bmvr_xr_sink_open/_client_begin/_pair/_client_end/_close)
linux/bmvr_pair_ring.h (new)  the 3-slot ring state machine, pure bookkeeping (no Vulkan), unit-tested with a fake GPU (tests/linux/ring_test.c)
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

**Amended 2026-11-21 (X2, real game): one queue was not enough.** With one queue and one mutex on SteamVR 2.19.4 + RTX 3080 the
transport thread waited 4 ms on average (10 ms max) for the lock at 30 pairs/s, and 18-21 ms on average (90-97 ms max) at
`sync=finish`, `every=1`, with the XR frame rate collapsing from 90/s to about 5/s. The Vulkan validation layer reported
`UNASSIGNED-Threading-MultipleThreads-Write` (one `VkQueue` inside `vkQueueSubmit` on two threads at once, despite `qmu`) as soon
frames were displayed, and the kernel logged NVIDIA Xid 32 (invalid or corrupted push buffer) and Xid 69 (class error, class
0xC797) against `bmvr_xfer_helper` in three sessions (13:09, 14:00, 14:14 local), each followed by SteamVR's compositor watchdog
(`WaitForPresent` hung 6.8 s) and a lost session. Which thread was the second submitter is **not proven**: the runtime lives in the
helper process and may submit on the session's queue from XR calls that are not under `qmu` (`xrWaitFrame`,
`xrAcquireSwapchainImage`, `xrWaitSwapchainImage`); the fix does not need to know.

**The fix (`linux/bmvr_xr_sink.c`):** the runtime-created device is requested with two queues of the graphics family (family 0 has
16 on the RTX 3080). Queue 0 stays the session's queue (XR thread and runtime). Queue 1 belongs to the transport thread alone and its
submits take no mutex. `qmu` is used only when the family has fewer than two queues or `BMVR_XR_ONE_QUEUE=1` is set (fallback and
A/B comparison). The helper logs `queues: transport thread on its own queue (family N has M)` at start; **if that line is missing
the installed helper is an old build** (`scripts/build-linux.sh` installs it). No extra synchronization is needed for the hand-over
between the two queues: the XR thread only picks a slot whose copy fence has signalled (`sk_done_fn`, `vkGetFenceStatus`), all
images are `VK_SHARING_MODE_EXCLUSIVE` in one family (no ownership transfer), and the ring's mutex orders the slot states.
After the fix the helper log shows no threading error, a transport lock wait of 0.000 ms and 89-90 XR frames/s throughout.

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
the engine renders, so a helper -> shim direction is required. Proposed **protocol v5** (**implemented in X4, see "X4: the pose channel"**; a v4 peer is rejected with both versions logged, like T18):

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
| X0 | `bmvr_xr_sink` skeleton, `--xr-probe`: instance, system, view config, swapchain formats, session create/destroy, no game, no frames | Monado **and** SteamVR | Logs runtime name/version/extensions/formats on both; confirms or corrects every *(verify)* above; helper exits cleanly. **Status: implemented; Monado 21.0.0 (sandbox) PASS; SteamVR 2.19.4 + RTX 3080 PASS (author's run)** |
| X1 | `--selftest-xr`: frame loop with a generated pattern (left tinted one way, right the other, frame counter), quad then projection | Monado sim/qwerty first, then a headset | Correct eye order; steady `xrWaitFrame` cadence; rate lines sane; no per-frame disk writes. **Status: implemented; SteamVR 2.19.4 + RTX 3080 PASS, "L" blue / "R" red seen in the headset (author's run); Monado not run** (see "X1 results") |
| X2 | Socket sink: pair -> ring -> swapchain, quad layer, eye 0 | game + runtime | **Status: COMPLETE (2026-11-21). Stand-in tests PASS on Monado + lavapipe; run and fully tested with the real game on SteamVR 2.19.4 + RTX 3080, `sync=sem`, two eyes, as reported by the author** (see "X2 results" and "X2 results (real game, SteamVR)"). Game picture visible in the headset on each runtime; `test_kill_loop.sh -n 10`, then 30, passes with `--sink xr` (helper killed at random points: game unaffected, helper-less period shows nothing or last image, reconnect restores the picture). **Recorded: kill loop 30/30 on the stand-in; against the real game with `--sink xr` not recorded** |
| X3 | Projection layer with a pinned pose; `target2=name:_rt_gblightdiff` (**implemented 2026-11-21; run with the real game on SteamVR (two swapchains, 89-90 XR frames/s, pairs 60/s, 0 dropped) but the pass criterion is NOT met: the engine renders one view, so with `target2=InternalRT0` both eyes carry the same flat picture, and with the per-eye poses/FOVs of the projection layer the two images do not overlap correctly (author's observation, expected for mono content). `--xr-layer quad` shows one picture to both eyes**: `--sink xr --eyes 2 --xr-layer projection`; two swapchains, eye 0 = left, eye 1 = right, head-locked VIEW space, runtime eye poses with pinned fallback) | as X2 | Left and right visibly different content, correct sides, no eye ever shows a different tick than the other |
| X4 | Protocol v5 pose channel, mock-tested (`mock_game` replays poses; new case T20+) | none (stand-ins) | **Status: implemented 2026-11-21; stand-in tests T21-T23 + `pose_test` PASS (native build). With the real game on SteamVR the poses reach the shim (first pose id received, LOCAL space, left eye at (-0.028 -0.001 -0.001), FOV l/r/u/d -1.029 0.882 0.869 -1.042 rad) and the `stale` counter rises when pairs slow or stop; the game does not render with the poses yet.** Pair slots carry their own `pose_id`; stale id handled; v4 peer rejected with both versions named. Real use waits for the hook port |

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
| Does the runtime offer `XR_KHR_vulkan_enable2`? | yes (also `XR_KHR_vulkan_enable`, `XR_KHR_composition_layer_depth`) | **SteamVR 2.19.4: yes** (43 extensions, incl. `XR_KHR_vulkan_enable`, `XR_KHR_composition_layer_depth`, `XR_MND_headless`) |
| Does the runtime-owned device flow work (requirements -> `xrCreateVulkanInstanceKHR` -> `xrGetVulkanGraphicsDevice2KHR` -> `xrCreateVulkanDeviceKHR` -> `xrCreateSession`)? | yes; Vulkan 1.1 instance accepted (Monado reports its upper bound as 1023.1023 = no limit) | **SteamVR: yes**; it accepts Vulkan 1.0 .. 1.2 (a finite window, unlike Monado) and a 1.1 instance worked |
| Is `R8G8B8A8_SRGB` offered, and does a swapchain in it with `TRANSFER_DST` usage work (the raw-copy assumption)? | yes: offered (also `R8G8B8A8_UNORM`, both BGRA, `R16G16B16A16_SFLOAT`), 3 images | **SteamVR: `R8G8B8A8_SRGB` offered and a 3036x3036 swapchain with `TRANSFER_DST\|SAMPLED` worked (3 images). `R8G8B8A8_UNORM` is NOT offered by SteamVR**, so the sRGB swapchain with a raw copy is the only RGBA8 route there (also offered: `B8G8R8A8_SRGB`, `R16G16B16A16_SFLOAT`, `A2B10G10R10_UNORM_PACK32`, depth formats) |
| Can the helper's exportable images live on the runtime's device? | yes on lavapipe (opaque-fd image export reported exportable) | **RTX 3080: yes**, image export reported exportable (opaque-fd, optimal tiling) |
| Is the swapchain limited to the view's `maxImageRect`? | no: a 3840x2160 swapchain was created although the simulated HMD reports max 1024x1024 per view | runtime-specific; do not rely on it. SteamVR: not tested (the probe used its recommended 3036x3036; max is 8192x8192) |
| Does the session reach `READY` without frames? | yes (`IDLE` -> `READY`) | **SteamVR: yes** (`IDLE` -> `READY`) |
| What does "runtime installed, service not running" look like? | `xrEnumerateInstanceExtensionProperties` still works, `xrCreateInstance` fails with `XR_ERROR_RUNTIME_FAILURE` (-2): reported as UNAVAILABLE with a hint | SteamVR will differ |
| Recommended eye size | the simulated HMD reports 128x128 | **SteamVR: 3036x3036 per eye** (left == right), max 8192x8192, recommended sample count 1 |
| Opaque-fd **semaphores** (`sync=sem`) on the runtime's device | not observable (lavapipe has none) | **RTX 3080: exportable and importable; `VK_KHR_external_semaphore_fd` and `VK_KHR_external_memory_fd` present** |
| Queue family for the runtime-created device | n/a | **family 0 with 16 queues** (the design's one queue + one mutex fits) |
| Runtime-chosen GPU, device UUID | lavapipe | **"NVIDIA GeForce RTX 3080" (Vulkan 1.4), UUID `afc5d13f9cb1b726f1b996abdf9a7c59`** (printed, not yet compared with the game's GL UUID) |
| Monado quirk | `monado-service` exits at startup if its stdin cannot be polled (`/dev/null`): the test gives it an open FIFO | n/a |

SteamVR column: one run on one machine (author's report, Linux, RTX 3080, system name `SteamVR/OpenXR : cv`, vendor 10462),
exit PASS, 2026-11-20. Needed one fix first: SteamVR was running but not the *active* OpenXR runtime (probe exit 2,
`failed to determine active runtime file path`); the Developer-tab button "Set SteamVR as OpenXR runtime" did not exist on that
install, and symlinking `steamxr_linux64.json` to `~/.config/openxr/1/active_runtime.json` fixed it (see `LINUX_PORT.md`).

Still open after both runtimes: the device UUID comparison with the game's GL context (printed, not compared yet), the
queue-sharing rule, and everything about frames (X1 onward).

## X1: `--selftest-xr`

```
bmvr_xfer_helper --selftest-xr [--xr-layer quad|projection|both] [--xr-seconds N] [--xr-wait MS] [--xr-size WxH]
```

Needs no game folder and no helper lock. SteamVR running and set as the active runtime (or `monado-service`), headset connected
(and for SteamVR on, so the session becomes visible). Defaults: `both` layers, 10 s each, wait up to 10 s for `READY`, the
runtime's recommended eye size. Exit codes as for the probe: 0 PASS, 1 FAILED, 2 UNAVAILABLE. Ctrl-C ends it cleanly.

What it does: opens the session exactly like the probe (same device flags), creates one `R8G8B8A8_SRGB` swapchain per eye
(first with `COLOR_ATTACHMENT` usage on top of `TRANSFER_DST|SAMPLED`, falling back to the flags X0 verified), uploads one pattern
image per eye **once** (`R8G8B8A8_UNORM`, like the helper's shared images), then loops `xrWaitFrame -> xrBeginFrame -> [acquire,
wait, GPU copies, release] -> xrEndFrame`. Per frame it only records GPU copies: pattern -> swapchain image (`vkCmdCopyImage`, the
design's raw copy, no shader) and 17 small tiles (a 16-bit frame counter and a moving marker) from a tiny buffer. One thread, one
queue, 4 command buffers with fences in flight. No per-frame disk writes, no per-frame log lines.

What you should see in the headset: **left eye BLUE with a white "L", right eye RED with a white "R"**, white border, grid lines,
at the top 16 squares counting frames in binary (white = 1) and at the bottom a yellow square moving left to right.
Quad phase: eye 0 (the "L" picture) on a head-locked 1.6 m quad, 2 m ahead, same picture in both eyes. Projection phase: both
eyes as a head-locked stereo projection layer (VIEW space, the runtime's own eye poses and FOVs from `xrLocateViews`; pinned poses
and a 90 degree FOV if the runtime gives none, logged once). A wrong eye order shows as red-left/blue-right; a dropped eye as one
eye black or showing the other colour; a split pair would show different counter values per eye (should not happen: both eyes are
drawn from the same frame number in one command buffer).

Output while it runs: session state changes, then one line per 5 s per phase:
`selftest[quad]: 90.0 fps (display period 11.11 ms), frame interval min/avg/max .../.../... ms, rendered N/N, late N, wait avg .. ms, cpu avg .. ms`
("late" = interval above 1.5 x the predicted display period; "rendered" = frames where `shouldRender` was true). The summary lists
`frames submitted` (required: at least one rendered frame per phase) and `cadence` (informational: average within +-15 % of the
period, maximum below 2.5 x the period).

What it does NOT check, and what is *(verify)* until a run on each runtime says otherwise:

- The picture itself (eye order, colours, counter): visual, by you. The program says so in its last lines.
- The hand-back layout: images are put in `COLOR_ATTACHMENT_OPTIMAL` before `xrReleaseSwapchainImage` and come in as `UNDEFINED`
  (contents are fully overwritten). The Vulkan binding's layout rule is from memory of the spec; a validation layer or a
  runtime complaint would show here *(verify)*.
- Whether `COLOR_ATTACHMENT` usage on the swapchain is accepted (the summary line says which flags were used).
- Queue use: one thread submits, the runtime may use the same queue inside `xr*` calls *(verify)*.
- Anything about the game, the pair ring (X2), poses (X4) or latency.
- Teardown after Ctrl-C or a runtime-ended session: handled in code (GPU idle first, then swapchains, space, session). The one SteamVR run
  went to the end and exited through `xrRequestExitSession`; the Ctrl-C path has not been observed.

`tests/linux/test_xr_selftest.sh` (Monado headless) is written but was not executed.

## X1 results

One run on SteamVR/OpenXR 2.19.4, NVIDIA RTX 3080, Linux (author's report, 2026-11-20), `--selftest-xr` with defaults (quad 10 s,
then projection 10 s), exit PASS. Headset: the author saw a blue square with an "L" and a red one with an "R". Not separately
confirmed: that blue was in the left eye and red in the right, the counter counting, the marker moving, the quad being identical
in both eyes.

| Question | Observed (SteamVR, one run) |
|---|---|
| Does the runtime accept the swapchain with `TRANSFER_DST\|SAMPLED\|COLOR_ATTACHMENT`? | yes, 2 swapchains of 3036x3036, 3 images each (no fallback to the X0 flags needed) |
| Does the raw copy (UNORM pattern -> SRGB swapchain, `vkCmdCopyImage`, no shader) show the right picture? | the author saw the expected blue "L" / red "R"; no colour or channel problem reported |
| Does handing the images back in `COLOR_ATTACHMENT_OPTIMAL` after transfer writes work? | it showed a picture and the runtime raised no error; no Vulkan validation layer was active, so spec conformance is not proven |
| Does one thread / one queue / 4 command buffers with fences work? | yes: no stall, GPU copy plus submit CPU time 0.05 ms per frame (quad) and 0.10 ms (projection, 2 eyes) |
| `xrWaitFrame` cadence | display period 11.11 ms (90 Hz); 895 quad frames averaged 11.18 ms (88.9 / 89.9 fps per 5 s window), 898 projection frames 11.15 ms; all frames had `shouldRender` true |
| Frame-interval outliers | one 70.7 ms interval in the first window of the quad phase and one 45.7 ms interval in the first window of the projection phase (the second windows were clean, max 13.4 / 13.2 ms). Cause not investigated; the program's `cadence` item is informational (max above 2.5 x period) and reported `note` for both |
| Session lifecycle | `IDLE` -> `READY` -> (begin) -> `SYNCHRONIZED` -> `VISIBLE` -> `FOCUSED` (reached during the projection phase) ; on exit request `VISIBLE` -> `SYNCHRONIZED` -> `STOPPING` -> `IDLE` -> `EXITING`; clean end |
| `xrLocateViews` in VIEW space for the projection layer | no "pinned poses" message was logged, so the runtime returned valid views |
| Log volume | 2 phases x 2 rate lines plus state changes; no per-frame lines (disk writes: none by construction, not measured) |

Still open after X1: the eye assignment and counter/marker checks above (a quick look with one eye closed settles the first),
the two startup interval spikes, Monado, a validation-layer run, the Ctrl-C path, and everything involving the game (X2 onward).

## X2: `--sink xr`

```
bmvr_xfer_helper [game folder] --sink xr [--eyes 1|2] [--no-semaphores] [--xr-size WxH] [--xr-wait MS] [--xr-check N] [--xr-quad world|head]
```

Same socket, hello, frame and ack as the snapshot sink (protocol v4 unchanged); `--sink snapshot` (default) is untouched. The
helper opens the OpenXR runtime first, **creates the shared images and semaphores on the runtime's Vulkan device**, and only then
listens. Image size = the runtime's recommended size unless `--xr-size`; `--size` and `--gpu` are ignored (logged). Unavailable
runtime / no headset / session not READY in `--xr-wait` ms (default 10 s) / missing `VK_KHR_external_memory_fd`: the helper
**exits with the reason (exit 2 unavailable, 1 failed) and does not fall back to snapshots**. `--xr-layer` other than quad is refused (X3).

Data path per pair, as designed in Decision 2:

1. Socket thread, per frame message: `bmvr_ring_begin_write` -> ONE queue submission `[wait gl_ready] -> copy shared eye(s) -> ring slot
   (vkCmdCopyImage, no shader) -> [signal vk_done]` with a per-slot fence -> `bmvr_ring_end_write` -> ack. With `sync=finish` (no
   semaphores) the ack follows after the copy's fence instead. If no slot can be had, the submission still waits/signals (no copy), so the
   game never stalls on a full ring; the pair is counted as dropped.
2. XR thread (pacing master): `xrWaitFrame` -> wait for the previous frame's GPU copy -> `xrBeginFrame` -> `bmvr_ring_pick` (the newest
   complete pair) -> copy its eye 0 into the acquired swapchain image -> `xrEndFrame` with a quad 2 m ahead, 1.6 m high (default `--xr-quad world`: fixed in the room, LOCAL space; `head`: VIEW space, follows the head). No newer pair: the same slot is copied again (`reused`). No pair ever: an empty frame. The game leaving does not stop
   this: the last pair stays on the quad, and a reconnect restores the live picture.
3. The ring (`linux/bmvr_pair_ring.h`): slots FREE / WRITING / SUBMITTED / READY, one mutex. A slot holds both eyes of one pair and is
   published as a whole; the reader sees it only after the GPU fence says the whole copy finished. The slot the XR thread displays
   is never recycled (also not by the writer, which recycles finished, superseded slots itself when it finds none free).
4. One queue, one mutex (`qmu`) around every `vkQueueSubmit` and around `xrBeginFrame`/`xrReleaseSwapchainImage`/`xrEndFrame`
   (the calls where a runtime may touch the application's queue, *(verify)*). The rate line reports how long the transport waited for it.
5. Output: one line per 5 s, `xr-sink: pairs=N/s displayed=N/s reused=N/s dropped=N/s empty=N/s | xr frames N/s | queue lock wait
   (transport) avg .. max .. ms`. No per-frame lines, no per-frame disk writes.

`--xr-check N` (debug) reads back 16 pixels of the first N fresh pairs at every stage (swapchain image, ring eye 0, ring eye 1) and logs
them. It stalls one frame per checked pair. It is what lets a test say that the bytes that arrive carry the pair's own tick.

### Tests (`tests/linux`, not part of `run-tests.sh`)

- `ring_test.c`: ring state machine with a fake GPU thread, five speed ratios, also under `-fsanitize=thread`: no half pair at any
  point of a display period, a displayed slot is never rewritten, the sequence never goes backwards, a pair is fresh at most once,
  everything accounted for. **Mutation-checked**: promoting without the GPU's `done`, and recycling the displayed slot, are both caught.
- `xr_client.c`: a Vulkan stand-in for the shim (imports the exported memory, writes tick-coded pixels, sends frames, reconnects, reports a
  HANG if an ack is missing for 5 s).
- `test_xr_sink.sh [build dir] [--software] [--kill-rounds N]`: cases X2-a..g against its own headless Monado.

### X2 results

Monado 21.0.0 (Ubuntu 24.04 package, `SIMULATED_ENABLE=1 XRT_COMPOSITOR_NULL=1`), Mesa lavapipe, no GPU, no headset, 2026-11-21:

| Check | Observed |
|---|---|
| Runtime unavailable | exit 2, "refusing to fall back to snapshots", no files, no socket |
| 1 eye, 157 frames at 40/s | 157 sent, 157 acked; `--xr-check`: in all 6 checked pairs the swapchain image and the ring slot carry the message's tick |
| 2 eyes, 156 frames | negotiated `eyes=2`; in all 6 checked pairs eye 0 and eye 1 carry the same tick (eye colours differ, so they really are two images) |
| Game leaves | rate lines continue with `pairs=0 displayed=20/s reused=20/s`: the last pair keeps being shown |
| Second helper, same folder | refused |
| SIGINT | exit 0, session goes through STOPPING, socket removed |
| `kill -9` of the helper while the game runs | **30 rounds**: the game process never hung, reconnected after every kill (31 connects, 1427 of 1455 pairs acked; the rest were in flight at a kill) |
| Sanitizers | ASan+UBSan: 16/16 clean. TSan: found a real (formal) race on the stop/dead flags and the `why` string, fixed with C11 atomics; clean afterwards |
| Regression | T1-T19 stand-in suite, X0 (14/14) and X1 (11/11) unchanged |

Also learned: lavapipe exports the opaque-fd memory and a second process **can import it** (the cross-process image sharing works on a
software driver); Monado's simulated HMD runs at 20 Hz (display period 50 ms), so `dropped` is high when the stand-in sends 40 pairs/s,
as designed (newest wins).

**What the stand-in run of X2 did NOT show** (written before the real-game run; the real-game results below settle the game, semaphore, SteamVR/headset and single-queue items; the rest stays open):

- The real game: the shim's GL import of these images and `sync=sem` / `sync=finish` against this helper. The stand-in cannot do
  GL, and lavapipe has no semaphore export, so **the semaphore path (wait gl_ready / signal vk_done on the runtime's queue) has not
  run at all**, not even against the stand-in.
- SteamVR, any real GPU, a headset: nothing was seen. Monado's null compositor shows no picture; what is verified is the bytes in the
  swapchain image, not that a runtime displayed them.
- The single-queue rule: whether the runtime uses the application's queue in calls that are not under `qmu` (e.g.
  `xrWaitSwapchainImage`, `xrLocateViews`) is unknown; the lock-wait numbers on a real runtime will say whether the mutex is cheap.
- Device UUID comparison between the runtime's GPU and the shim's GL context: the helper **prints** the runtime's UUID at start;
  the comparison needs a hello addition and is still open.
- Performance: the copy cost at 3036x3036 per eye (the design's "~1 ms class" is still an estimate), frame pacing against a 90 Hz compositor.
- Known limit: if a game dies after signalling nothing for a pair it already announced (never signalled `gl_ready`), the transport queue
  stays blocked; the helper detects it (bounded waits), says so and asks for a restart (same hazard as the snapshot sink's 5 s check).

### X2 first run with the real game (author's report, SteamVR, RTX 3080, 2026-11-21)

Reported, not measured (no logs were attached yet):

- `--sink xr --eyes 1 --xr-check 6`: the quad is visible in the headset and the game image **updates every 5-6 s**.
  Cause found in the code, **not yet confirmed by a run**: the shim delivers one pair every `XFER_EVERY` = 120 presents (a snapshot-era
  constant, about 2 s at 60 fps, 5-6 s at this game's frame rate). The sink shows the newest pair on every XR frame; it just gets few.
  Change made afterwards (tested host-native with the mock GL, T20; **not tested in the 32-bit shim or the real game**): `bmvr_target.txt` key
  `every=N` (1..3600, default stays 120). Try `every=1`, then `every=2` if the engine cost is too high.
- `--sink xr --eyes 2` with `target=InternalRT0`, `target2=name:_rt_gblightdiff`, `flip=0`, `sync=sem`: the same image in both eyes (expected: X2
  shows eye 0 on a quad, stereo is X3) and the view "stuck" (expected: the quad was head-locked). Change made afterwards: `--xr-quad world`
  (new default; the quad is fixed in the room, LOCAL space, 2 m ahead of the head's position at session start) and `--xr-quad head` for the old
  behaviour. Checked only that both start on Monado. The author also saw the game as a "floating desktop application" in the headset; that is
  not explained (probably SteamVR's own desktop/window overlay of the game window, not this sink) and has not been investigated.
- Not yet reported: whether `sync=sem` delivered pairs at all in this run, the helper's rate/check lines, the UUID lines.

### X2 `sync=sem` crash (author's logs, 2026-11-21)

`sync=finish`: stable through a level load. `sync=sem`: after ~65 s of normal running (25-30 pairs/s, no drops), during a level load
(game 14.7 fps, 600+ CreateTexture per window, then frames with 0 SetRenderTarget), the helper's `vkQueueSubmit` returned
`VK_ERROR_DEVICE_LOST` (-4) and SteamVR showed Fail -203. **Cause not established.** Suspects: (1) the transport's wait(gl_ready)
submission blocks the single shared VkQueue while GL is slow, stalling the XR thread's copy behind it; (2) async lifetime of the source
texture without glFinish; (3) SteamVR 2.19.4 beta / NVIDIA 620.73 with imported opaque-fd binary semaphores. Bisect: `sem` + extra
`glFinish` after the signal; separate VkQueue for the transport; `journalctl -k | grep -i xid`; Vulkan validation layer on the helper.
Use `sync=finish` for X3 until this is resolved.

### X2 results (real game, SteamVR)

Reported by the author as complete and fully tested, 2026-11-21. Setup: Black Mesa (Proton) with the shim,
`bmvr_xfer_helper --sink xr --eyes 2 --xr-layer projection`, SteamVR/OpenXR 2.19.4, NVIDIA RTX 3080 (open kernel module 620.73.04),
headset on SteamVR via vrlink; `bmvr_target.txt`: `target=InternalRT0`, `target2=InternalRT0`, `flip=0`, `sync=sem`, `every=1`. The
numbers are from the shim and helper logs of those runs. The length of the soak, the Xid check of the final runs and a kill loop with
`--sink xr` against the real game are not in the logs attached to this record.

| Check | Observed |
|---|---|
| Two eyes through GL import | shim: "imported the helper's Vulkan memory as GL textures 997 (eye 0) and 998 (eye 1)"; shared image 3036x3036, 37355520 bytes per image |
| GPU semaphores (`sync=sem`) | "GL semaphores 1/2 imported", no `glFinish`; this path had never run before, not even against the stand-in |
| Cost per pair | GL side 122-137 us, helper round trip 107-128 us (averages over 10-50 snapshots) |
| Pacing | game about 60 fps; XR 89-90 frames/s; pairs 60/s, displayed 90/s, reused 30/s, dropped 0.0/s in steady state (newest wins, reuse when starved, as designed) |
| Queue contention | single queue: failed (Decision 2, amended); own transport queue: lock wait 0.000 ms, no threading error from the validation layer |
| Pose channel | the shim receives poses (see the X4 row); the game does not use them yet |
| Same GPU | the helper prints the runtime's device UUID `afc5d13f9cb1b726f1b996abdf9a7c59`, equal to the kernel's `GPU-afc5d13f-9cb1-b726-f1b9-96abdf9a7c59` (one GPU in the machine); the automatic shim/helper UUID comparison is still not implemented |
| Session states | SYNCHRONIZED / VISIBLE / FOCUSED alternate when the headset is taken off and put on; the XR loop keeps running at 89-90 frames/s through them |

**Failures found on the way (before the queue fix), for the record:** NVIDIA Xid 32 (twice per event) at 13:09 (pid 13879) and 14:00
(pid 31481) and Xid 69 + Xid 32 at 14:14 (pid 33548), all `bmvr_xfer_helpe`; the compositor then logged `WaitForAcquire timed out` about
90 times a second and its watchdog aborted in `WaitForPresent` after 6.8 s, which ended the session.

**Operational notes learned:**

- **Wake the headset before starting the helper.** With the headset in standby SteamVR got no vsync from vrlink, `WaitForPresent`
  aborted every second, `vrserver`'s `Connection` thread watchdog fired after 21 s and SteamVR exited, with no game running and no
  Xid. This is not a helper fault.
- After changing `linux/`, run `scripts/build-linux.sh` (it builds from scratch and installs the helper), then check the `queues:` line.
- The Vulkan validation layer, when enabled for the helper (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`), prints shader-module
  (`ShaderViewportIndexLayerEXT`) and `vkCmdDraw` image-layout errors; the helper has no shaders or draw calls, so these come from the
  SteamVR runtime in the helper process and can be ignored. When the runtime ends the session the helper exits without destroying its
  Vulkan objects (the layer reports about 315 leaked objects); harmless at exit, not cleaned up.
- Check for GPU faults after a session with `sudo dmesg | grep -i xid` (the `r8169 ... XID 641` line is the network card).
- Validation slows the helper a lot at 3036x3036 per eye; use `every=3` while it is enabled.

**Still open after X2:** a kill loop with `--sink xr` against the real game; a `sync=finish` soak after the queue fix (the crashing runs
used `sync=finish`); the GPU copy time per frame at 3036x3036 (only the round trip above is measured); motion-to-photon (needs poses and
the hook port); a mono-overlap mode for the projection layer (same pose and one FOV for both eyes) until real stereo exists; poses going
`stale` while the game is paused or loading (no projection layer is submitted for such a slot).

## X4: the pose channel (protocol v5)

Implemented and stand-in tested; run with the real game on SteamVR 2.19.4 (2026-11-21): the pose channel delivers poses to the shim.
The game does not render with them yet (that is the hook port), so the pose id of a pair names a pose its picture was not rendered from.

**Deviation from Decision 5:** there is no separate helper -> shim message. The socket stays strictly request/response (the shim only
ever blocks in `recv` for an ack), so every **ack carries the helper's newest pose** (`struct bmvr_xfer_ack.pose`, `pose_id` 0 = none).
Poses reach the game at the delivery rate (`every=N`), which is also the rate at which the game could use them.

```
hello.caps  BMVR_XFER_CAP_POSES (2)        helper offers poses (only --sink xr --xr-layer projection with a LOCAL space)
ack         + struct bmvr_xfer_pose        pose_id, flags (POSE_LOCAL), display_time, eye[2]{pos[3], rot[4], fov[4]}   (112 bytes total)
frame       + pose_id                      "this pair was rendered with pose pose_id" (96 bytes total)
```

- **Helper:** the XR thread locates both eyes in LOCAL space for every `shouldRender` frame (`xrLocateViews`, the frame's predicted
  display time) and puts the result in the pose table (`linux/bmvr_pose_store.h`: last 32 poses by id, mutex, stale counter; no
  Vulkan/OpenXR, unit-tested). The transport thread reads the newest for each ack. The transport records each pair's `pose_id` in
  `slot_pose[slot]` before `bmvr_ring_end_write`; the XR thread reads it after `bmvr_ring_pick` (the ring's mutex orders the two).
- **Layer:** pair with `pose_id` N: the projection layer uses exactly pose N's eye poses and FOVs, in LOCAL space, so the compositor
  reprojects from where the frame was rendered. `pose_id` 0 (game without poses): head-locked as in X3. N no longer stored: no layer
  that frame, counted (`stale=` in the rate line, shown as `poses issued=.. stale=..` when poses are on).
- **Shim:** keeps the pose of the newest ack. `xfer_tick` latches ids one Present apart: the frame delivered now was rendered with the id
  latched at the previous Present; the next frame starts with the newest. `bmvr_get_render_pose()` (exported) returns the newest pose for
  the hook port; nothing calls it yet, so today the ids are real but the picture is still rendered with the game's own camera.
- **Versions:** `BMVR_XFER_VERSION` is 5. A v4 helper/shim pair is refused by the shim's hello check with both versions in the log.
- **Not done / open:** converting OpenXR poses to Source space and the off-centre projection (hook port); a depth layer; what the
  compositor does with a pose that is several frames old (the pose is located at `predictedDisplayTime` of the frame it was issued on).
- **Tests:** `tests/linux/pose_test.c` (table: ids, stale, wrap, counters, writer/reader stress, TSAN clean) and suite cases T21-T23 with
  `mock_helper --poses` (first frames carry pose 0, ids never decrease, never ahead of what the helper sent, 2- and 1-eye games, helper without poses).

## Risks and open questions

1. ~~**Queue synchronization with the runtime**~~ **Resolved in X2 (real game):** one queue behind one mutex was not enough (validation
   threading errors, Xid 32/69, compositor watchdog). The transport thread has its own queue (Decision 2, amended). Not proven: which
   XR call submits on the session's queue inside the runtime.
2. **Swapchain format availability**: holds on Monado 21.0.0 (X0, sandbox) and on SteamVR 2.19.4 (X0, author's run: sRGB offered,
   UNORM not offered, so the sRGB raw copy is the only RGBA8 route there). Not checked: other SteamVR versions, other GPUs.
3. **Latency with mono + pinned pose**: irrelevant to a head-locked quad, but it means this stage cannot say anything about
   motion-to-photon. That is only measurable once poses flow (X4 + hook port).
4. **SteamVR on Linux session lifecycle** (session state READY/FOCUSED transitions, losing focus while the game window has it):
   needs a headset run to know; the XR thread must treat `XR_SESSION_STATE_*` as a state machine, not assume FOCUSED. Observed (X2, real
   game): the states alternate when the headset is taken off and put on, and the loop keeps running; a headset in standby when the helper
   starts makes SteamVR's own watchdog kill SteamVR (see "Operational notes learned").
5. **Helper death while the session is running**: the compositor keeps the last submitted layer or drops it, runtime-dependent
   *(verify)*; the XR thread should keep submitting frames from the last READY slot while the game connection is down, so a
   reconnect does not look like a session restart.
6. **Extension/ABI drift of the OpenXR loader on the user's distro**: dlopen `libopenxr_loader.so.1` and call
   `xrGetInstanceProcAddr` for everything, as the helper does for Vulkan; log and exit if the loader is missing.
7. **Eye-count assumptions**: `--eyes 2` must be required for `--sink xr` projection mode; quad mode works with `--eyes 1`.

## Order of work

1. ~~Record the `_rt_gblightdiff` result in `LINUX_PORT.md`~~ (recorded as reported by the author; the snapshot numbers are still to be added).
2. ~~X0: run on Monado and on SteamVR~~ (done, see "X0 results").
3. ~~X1: run `--selftest-xr` on SteamVR~~ (done, see "X1 results"). X2 is implemented and stand-in tested (see "X2 results"). ~~Run X2 with the real game on SteamVR~~ **done 2026-11-21, as reported by the author** (see "X2 results (real game, SteamVR)"; open: `sync=finish` soak and the kill loop with `--sink xr` against the real game). **Next: X3 needs real stereo content**: stage 4b (`docs/LINUX_PORT.md`) now draws two views with the poses of the pose channel (implemented and stand-in tested; **not yet run against the real `client.so` or in a headset**), so X3 can be judged once that run exists. Without `stereo=1` the projection layer still shows one flat picture to both eyes.
4. Only then is `docs/LINUX_PORT.md`'s plan item 3 ("hand both to a native OpenXR helper") marked done.
