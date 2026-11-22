# Linux port: two-eye hand-off (stage 3d)

Status: **transport implemented, stand-in tested (protocol v4, T14-T19, host-native build) and verified in the real game** (pairs delivered on an RTX 3080, kill loop 30/30 with `--eyes 2`, 32-bit suite passes). **Eye 1 carrying
different content is not proven yet**: with `target2=name:_rt_hud` every eye-1 pixel was (0,0,0), probably an empty HUD layer (menu and
loading screens); `_rt_gblightdiff` and a HUD-visible run are the next tests. **Stereo content is solved as of 2026-11-22** (stage 4b, `docs/LINUX_PORT.md` "Stage 4b real runs": inline-detoured `RenderView` draws two eyes; with `+mat_queue_mode 0` head tracking and depth were confirmed in the headset by the user). The sentence that follows describes the state before that. The OpenXR consumer exists and shows the game's pair in the headset (X2, complete 2026-11-21, `docs/LINUX_OPENXR_CONSUMER.md`), but with one engine view both eyes carry the same picture.
Read `docs/LINUX_PORT.md` first (stages 2a-3c) and `docs/openxr-eye-publish.md` (the Windows pipeline whose defects this design
must not repeat). How to try it in the game: the end of the "Stage 3d" section of `docs/LINUX_PORT.md`.

## What exists and what is missing

| Piece | State |
|---|---|
| Shim -> helper transport of ONE render target, GPU semaphores, helper-death guard, reconnect | verified in the real game (3a, 3b kill loop) |
| One helper per game folder (`flock`) | 3c, unit-tested; real-helper check pending |
| **Two eye images in the transport, a pair as one atomic publication** | **3d, this document; stand-in tested; real game verified (transport); eye-1 content pending** |
| Engine rendering a separate left/right view | **stage 4b: verified in the real game (headset, 2026-11-22), needs `mat_queue_mode 0`** (`docs/LINUX_PORT.md` "Stage 4b": RTTI-found `RenderView` vtable slot, two views from the pose channel, per-eye mid-frame blit, one pair). Still open: the real `client.so` (class, slot, `CViewSetup` layout) |
| A native OpenXR consumer | **does not exist**: `OpenXRHelper64/` is Windows-only (`XR_USE_GRAPHICS_API_D3D11`, `<Windows.h>`, 10k lines). A Linux consumer is a new program, not a port |

On the real game a pair is currently two *existing* render targets (or one target twice): that proves the plumbing, the
semaphores and the failure handling with known-good content. It does not produce stereo.

## Why a pair is one message and one GPU hand-off

Lesson from `openxr-eye-publish.md` (defect 2, "non-atomic left/right descriptor publish"): two eyes taken from different
game frames look like ghosting while the head moves. So the pair is never two independent publications.

```
GL side     : wait(vk_done) [not before the first pair] -> blit eye 0 -> blit eye 1 -> signal(gl_ready) -> flush -> frame message
helper side : wait(gl_ready) -> read both images -> signal(vk_done)  (queue submit; ack is sent after the submit)
```

ONE semaphore pair covers both eyes: both images of a pair are guaranteed to belong together, and the hang-risk analysis of
3a/3b (a GPU wait has no timeout, so the shim checks the helper's socket before waiting) is unchanged instead of doubled.

## Protocol v4 (`linux/bmvr_xfer.h`; shim and helper must be rebuilt together)

```
hello  (48 bytes, was 40): ... caps, eye_count (1 or 2 = images offered), reserved, memory_size (of EACH image)
       fds: memory[0], [memory[1]], then (if CAP_SEMAPHORES) gl_ready, vk_done
frame  (92 bytes, was 48): ... name[24] (eye 0), eye_count (1 or 2 = images written),
       eye1_src_w, eye1_src_h, eye1_flags, eye1_gl_name, eye1_name[24]
       flags: BMVR_XFER_FLAG_SEMAPHORE applies to the whole pair; FLIPPED/WINDOW are per eye (eye1_flags)
ack    : unchanged, ONE per pair
```

Both structs use fixed-width fields only, and `memory_size` sits at offset 40 (after an explicit `reserved`), so the layout is
identical in the 32-bit shim and the 64-bit helper (`_Static_assert` on both sizes; checked on the 64-bit side here, the i386 side by
the rule that offset 40 is already 8-aligned). A frame with `eye_count == 1` on a two-image session touches image 0
only. A hello with `eye_count` outside 1..2, or a frame whose `eye_count` exceeds what the helper offered, is rejected.

Negotiation (shim side): eye 1 is used only when `target2` is configured AND the helper offers two images. Every other
combination (helper offers 2 / no `target2`; `target2` / helper offers 1; eye 0 = window) logs the reason and delivers eye 0
only, exactly like the earlier stages; an unused second memory fd is closed.

## Shim (`linux/bmvr_preload.c`)

- Config: `target2=name:<label>|size:WxH`, `flip2=0|1|alt` (default = `flip`) in `bmvr_target.txt`; env `BMVR_TARGET2`,
  `BMVR_FLIP2`. `target2=window` is refused. With `flip=alt` both eyes of a pair use the same orientation.
- Capture state is per eye (`g_cap[]`, `g_last_good[]`); one `SetRenderTarget` leave can fill both eyes (same object twice).
- A pair is delivered only when BOTH eyes were captured in the same armed frame (the loading/menu fallback of 3b applies per
  eye and only when both eyes have a last known object). Otherwise nothing is sent, the partial capture is dropped and the log
  says which eye was missing, so a half pair can never reach the helper.
- Two imported textures, two draw FBOs, one read FBO; the semaphore barrier lists name both textures (`nt=2`).
- Teardown, the helper-death check of 3b and the reconnect logic cover both images.

## Helper (`linux/bmvr_xfer_helper.c`)

- `--eyes 1|2` (default 1): two images, two readback buffers, ONE semaphore pair, one command buffer copying both images.
- Snapshots of a pair: `bmvr_snapshot_<n>_eye0_<what>_flip<f>.ppm` and `..._eye1_...`; single-eye names unchanged.
- `--selftest-sync` runs with the configured number of eyes.

## Tests

`tests/linux` (no GPU): `mock_helper --eyes N` (protocol v4), `mock_gl` traces both barrier textures, `fake_game` can stop
rendering one target (`FAKE_SKIP_RT` / `FAKE_SKIP_AFTER`).

| Case | What it pins down |
|---|---|
| T14 | sem pair: `WAIT BLIT BLIT SIGNAL FLUSH frame ack`; exactly one signal per pair, one wait per later pair; both textures in every barrier; two distinct blits (different sources, different shared images); no `glFinish` |
| T14b | finish pair: `BLIT BLIT FINISH frame ack`, no semaphore calls |
| T14c | eye 1 chosen by size, own flip (frame flags and blit destination) |
| T15 | eye 1 stops being rendered: no frame at all afterwards (never eye 0 alone), no stray blit, the missing eye is named |
| T16 | negotiation: helper offers 2 / no target2; target2 / helper offers 1; target2 with an eye-0 window |
| T17 | helper exits right after acking a pair: no GL wait, blit or signal afterwards, semaphores deleted, GL state clean |
| T18 | v3 peer rejected with both versions named; `eye_count` 3 rejected; the game is unaffected |
| T19 | loading/menu frames: both eyes reuse their last object, pairs keep arriving, no miss reported |

T1-T13 (single eye) pass unchanged. The cases ran as a host-native build (`BMVR_TEST_NATIVE=1`); the 32-bit run on the
author's machine is the authoritative one. The optional real-helper section of `run-tests.sh` also sends two pairs to the
real helper (`--eyes 2`) when a Vulkan device exists; that path has only been compiled, not run.

## Real-game verification order

1. `target2` = the same object as `target` (e.g. both `InternalRT0`): proves two images, the one semaphore pair and the
   snapshot files with known-good content.
2. `rt-trace` output decides a real second target; with one engine view both eyes look alike, which is expected.
3. `tests/linux/test_kill_loop.sh -n 10 --eyes 2` (fails if the shim fell back to one eye), then the default 30.

Done so far (2026-11-20, see `docs/LINUX_PORT.md`): step 1; the kill loop 30/30 with two eyes; step 2 with `target2=name:_rt_hud`
(transport fine, eye 1 all black, probably no HUD on screen); `target2=name:_rt_gblightdiff` reported completed and successful by the
author (numbers not yet recorded). Optional: `_rt_hud` with the HUD visible. The consumer is stage 4a: `docs/LINUX_OPENXR_CONSUMER.md`.

## The consumer (separate from the transport; needs a headset or a runtime to test)

A new 64-bit Linux program with a Vulkan graphics binding (`XR_KHR_vulkan_enable2`). The helper already owns a Vulkan device and
exportable images, so it can copy its two images into the OpenXR swapchain images on the GPU, without CPU readback. Open questions:
which runtime is the target (SteamVR on Linux, Monado); how the head pose gets back to the game (the shim -> game direction does
not exist yet: the per-eye view/projection must be known BEFORE the engine renders, so pose flow is game-hook work); and the pacing
model (Windows ran a ~200 fps game against a 90 Hz compositor with a publish-slot ring: `openxr-eye-publish.md`, pipeline steps 2-4).
