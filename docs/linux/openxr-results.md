# OpenXR bring-up and results (X0-X4)

Design: [openxr-helper.md](openxr-helper.md). Milestones are run on their own; a status is written down only when observed.

| | Deliverable | Status |
|---|---|---|
| X0 | `--xr-probe`: instance, system, view config, formats, session create/destroy, no frames | **PASS** on Monado 21.0.0 (sandbox, no GPU) and on SteamVR 2.19.4 + RTX 3080 (author's run) |
| X1 | `--selftest-xr`: generated pattern (left BLUE "L", right RED "R", frame counter), quad then projection | **PASS** on SteamVR 2.19.4 + RTX 3080 (about 90 fps; author saw blue "L" and red "R"). Monado 11/11 with the script |
| X2 | `--sink xr`: socket sink, pair ring, XR thread, quad layer | **complete (2026-11-21)**: stand-in tests on Monado + lavapipe (kill loop 30/30, ASan/UBSan/TSan clean); real game on SteamVR (author-reported, log-backed numbers below) |
| X3 | projection layer, two swapchains, eye 0 = left, eye 1 = right | runs with the real game (two swapchains, 89-90 XR frames/s, pairs 60/s, 0 dropped). Its original criterion needed real stereo content, which stage 4b supplies; the stereo picture itself was confirmed by the user, see [stereo-views.md](stereo-views.md) |
| X4 | pose channel (protocol v5) | implemented, stand-in tested (T21-T23, `pose_test`); poses reach the shim on SteamVR; used by the stereo hook since 4b |

## Making SteamVR the active OpenXR runtime

**A running SteamVR is not enough.** Without an active runtime the probe prints
`RuntimeManifestFile::FindManifestFiles - failed to determine active runtime file path`,
`xrEnumerateInstanceExtensionProperties failed (-51)` and ends `UNAVAILABLE`. SteamVR's Settings -> Developer button "Set SteamVR as
OpenXR runtime" was missing on the tested install. This worked:

    mkdir -p ~/.config/openxr/1
    ln -sf ~/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json ~/.config/openxr/1/active_runtime.json

- Other Steam library: adjust the path (check `steamxr_linux64.json` exists). Flatpak Steam: paths are under
  `~/.var/app/com.valvesoftware.Steam/` and the sandbox needs access to the OpenXR config. Per launch instead of system-wide:
  `XR_RUNTIME_JSON=<SteamVR dir>/steamxr_linux64.json`. Flatpak and the per-launch override are untested.
- A headset (or null driver) must be connected to SteamVR, otherwise a "no system" error can follow.
- **Wake the headset before starting the helper.** With the headset in standby SteamVR got no vsync from vrlink, `WaitForPresent`
  aborted every second and the `vrserver` watchdog killed SteamVR after 21 s, with no game running and no Xid. Not a helper fault.
- Monado: `XR_RUNTIME_JSON=<openxr_monado.json>`, run `monado-service` first. Headset-free: `SIMULATED_ENABLE=1` (or
  `QWERTY_ENABLE=1`) with `XRT_COMPOSITOR_NULL=1`. `monado-service` exits at startup if its stdin cannot be polled (`/dev/null`); the
  tests give it an open FIFO.

The helper logs `xrGetInstanceProperties`, enabled extensions and swapchain formats on every start and does not special-case a
runtime by name.

## X0: `--xr-probe [--xr-wait MS] [--xr-size WxH]`

Needs no game, no game folder, none of the helper's own Vulkan setup. Prints every step, then a verdict: exit 0 PASS, 1 a required item
missing, 2 UNAVAILABLE (no loader, no runtime, service not running, no headset system). `tests/linux/test_xr_probe.sh [--software]`
runs it against its own headless Monado (14/14 checks, 10 probes in a row exit 0). Try it: `"$GAME/bmvr_xfer_helper" --xr-probe`.

| Question | Monado 21.0.0 (sandbox, lavapipe) | SteamVR 2.19.4, RTX 3080 (one run) |
|---|---|---|
| `XR_KHR_vulkan_enable2` offered | yes | yes (43 extensions) |
| runtime-owned device flow works | yes; Vulkan 1.1 accepted | yes; accepts Vulkan 1.0..1.2 |
| `R8G8B8A8_SRGB` swapchain with `TRANSFER_DST` | offered, works (3 images) | offered, 3036x3036 worked; **`R8G8B8A8_UNORM` NOT offered** (also `B8G8R8A8_SRGB`, `R16G16B16A16_SFLOAT`, `A2B10G10R10_UNORM_PACK32`, depth) |
| helper's exportable images on the runtime's device | yes | yes (opaque-fd, optimal tiling) |
| session reaches READY without frames | yes | yes |
| recommended eye size | 128x128 (simulated HMD) | **3036x3036**, max 8192x8192 |
| opaque-fd semaphores | not observable | exportable and importable; `VK_KHR_external_semaphore_fd` / `_memory_fd` present |
| queue family of the runtime-created device | n/a | family 0 with 16 queues |
| GPU / UUID | lavapipe | "NVIDIA GeForce RTX 3080" (Vulkan 1.4), UUID `afc5d13f9cb1b726f1b996abdf9a7c59` |
| runtime installed, service not running | `xrCreateInstance` -> `XR_ERROR_RUNTIME_FAILURE` (-2), reported UNAVAILABLE | differs |

SteamVR system name `SteamVR/OpenXR : cv`, vendor 10462. A swapchain larger than the view's `maxImageRect` was accepted on Monado; do
not rely on it.

## X1: `--selftest-xr [--xr-layer quad|projection|both] [--xr-seconds N] [--xr-wait MS] [--xr-size WxH]`

Needs no game folder and no lock. Opens the session like the probe, creates one `R8G8B8A8_SRGB` swapchain per eye, uploads one pattern
per eye once and loops `xrWaitFrame -> xrBeginFrame -> [acquire, wait, GPU copies, release] -> xrEndFrame` on one thread and one queue.
Expected in the headset: left eye BLUE with white "L", right eye RED with white "R", border, grid, 16 squares counting frames in
binary at the top, a yellow square moving left to right at the bottom. A wrong eye order shows as red-left/blue-right; a dropped eye as
a black eye. Output: session states, one line per 5 s per phase (`fps`, interval min/avg/max, rendered, late, wait, cpu).

SteamVR run (2026-11-20): swapchains 2 x 3036x3036, 3 images each, accepted with `TRANSFER_DST|SAMPLED|COLOR_ATTACHMENT`; 895 quad and
898 projection frames at 11.18 / 11.15 ms (period 11.11 ms), all `shouldRender`; GPU copy plus submit CPU time 0.05 ms (quad), 0.10 ms
(projection); lifecycle `IDLE -> READY -> SYNCHRONIZED -> VISIBLE -> FOCUSED` and a clean exit through `xrRequestExitSession`. One
70.7 ms and one 45.7 ms interval in the first windows (cause not investigated). Not confirmed: that blue was in the left eye, the counter
counting, the Ctrl-C path, a validation-layer run. `test_xr_selftest.sh` (Monado) was later executed: 11/11.

## X2: `--sink xr [--eyes 1|2] [--no-semaphores] [--xr-size WxH] [--xr-wait MS] [--xr-check N] [--xr-quad world|head]`

Same socket, hello, frame and ack as the snapshot sink. The helper opens the runtime first, creates the shared images and semaphores on
the runtime's device, then listens. `--size` and `--gpu` are ignored. Data path per pair: socket thread submits
`[wait gl_ready] -> copy eye(s) -> ring slot -> [signal vk_done]` with a per-slot fence, then acks (with `sync=finish` the ack follows
the fence). If no slot can be had the submission still waits/signals and the pair is counted as dropped, so the game never stalls on a
full ring. XR thread: `xrWaitFrame` -> previous copy done -> `xrBeginFrame` -> pick newest complete pair -> copy into the swapchain image
-> `xrEndFrame`. The game leaving does not stop this: the last pair stays on screen.

`--xr-check N` (debug) reads back 16 pixels at every stage for the first N pairs (stalls one frame per checked pair).

### Stand-in results (Monado 21.0.0 + lavapipe, 2026-11-21)

Unavailable runtime: exit 2, no files, no socket. 1 eye, 157 frames: all acked, tick intact through swapchain and ring. 2 eyes: both carry
the same tick. Game leaves: `pairs=0 displayed=20/s reused=20/s`. Second helper refused. SIGINT: exit 0. `kill -9` loop **30 rounds**:
the game never hung, reconnected every time. ASan+UBSan 16/16 clean; TSan found a real race on the stop/dead flags and the `why`
string, fixed with C11 atomics. Learned: lavapipe exports opaque-fd memory and a second process can import it; Monado's simulated HMD
runs at 20 Hz. **Not shown by stand-ins:** GL import, `sync=sem`, any real GPU, SteamVR, a displayed picture, copy cost at 3036x3036.

### First real-game runs (author's report, 2026-11-21)

- `--eyes 1 --xr-check 6`: quad visible in the headset but the picture updated every 5-6 s. Cause: `XFER_EVERY` was 120 presents (a
  snapshot-era constant). Fix: `every=N` in `bmvr_target.txt` (use `every=1`).
- `--eyes 2` with a second target: same image in both eyes (expected) and a "stuck" view (head-locked quad). Fix: `--xr-quad world`.
  The author also saw the game as a "floating desktop application" in the headset: unexplained, probably SteamVR's own window overlay.
- `sync=finish`: stable through a level load. `sync=sem` before the queue fix: after ~65 s, during a level load (game at 14.7 fps),
  `vkQueueSubmit` returned `VK_ERROR_DEVICE_LOST` and SteamVR showed Fail -203. The cause was the shared queue (below).

### X2 results with the real game on SteamVR (2026-11-21)

Setup: native Black Mesa with the shim, `bmvr_xfer_helper --sink xr --eyes 2 --xr-layer projection`, SteamVR/OpenXR 2.19.4, RTX 3080
(open kernel module 620.73.04), headset via vrlink; `target=InternalRT0`, `target2=InternalRT0`, `flip=0`, `sync=sem`, `every=1`. (The
old record said "(Proton)"; the shim only exists for the native build, so that was a slip.)

| Check | Observed |
|---|---|
| Two eyes through GL import | GL textures 997 (eye 0) and 998 (eye 1); shared image 3036x3036, 37355520 bytes each |
| GPU semaphores | "GL semaphores 1/2 imported", no `glFinish`; this path had not run before, not even against a stand-in |
| Cost per pair | GL side 122-137 us, helper round trip 107-128 us |
| Pacing | game ~60 fps; XR 89-90 frames/s; pairs 60/s, displayed 90/s, reused 30/s, dropped 0.0/s |
| Queue contention | single queue failed; own transport queue: lock wait 0.000 ms, no threading error |
| Same GPU | runtime UUID `afc5d13f...` equals the kernel's `GPU-afc5d13f-9cb1-b726-f1b9-96abdf9a7c59` (one GPU) |
| Session states | SYNCHRONIZED / VISIBLE / FOCUSED alternate with the headset off/on; loop keeps running |

Failures before the queue fix: Xid 32 (twice per event) at 13:09 and 14:00, Xid 69 + Xid 32 at 14:14 (all `bmvr_xfer_helpe`), then
`WaitForAcquire timed out` ~90 times a second and the compositor watchdog abort after 6.8 s.

Operational notes: after changing `linux/` run `scripts/build-linux.sh` and check the `queues:` line; with
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` the shader-module and `vkCmdDraw` layout errors come from the SteamVR runtime and can
be ignored, and use `every=3` because validation is slow at 3036x3036; check for GPU faults with `sudo dmesg | grep -i xid` (the
`r8169 ... XID 641` line is the network card). When the runtime ends the session the helper exits without destroying its Vulkan
objects (harmless at exit).

**Still open after X2:** a kill loop with `--sink xr` against the real game; a `sync=finish` soak after the queue fix; GPU copy time per
frame; motion-to-photon; poses going `stale` while the game is paused or loading (no layer for such a slot).

## X4: the pose channel (details)

Poses reach the game at the delivery rate because every ack carries the newest one. The shim keeps the newest and `xfer_tick` latches ids
one Present apart. `bmvr_get_render_pose()` returns the pose the current pair was rendered with, not the newest (the newest is one ack
newer than the id that gets stamped, which would reproject from the wrong pose). First real poses on SteamVR: LOCAL space, left eye at
(-0.028 -0.001 -0.001), FOV l/r/u/d -1.029 0.882 0.869 -1.042 rad. Tests: `pose_test.c`, T21-T23.
