# Stereo views and head tracking (stage 4b)

Status: **stereo and head tracking verified in the headset 2026-11-22** (user report; no depth measurements) with the inline hook and
`mat_queue_mode 0`. The installer defaults turn it on (`stereo=1`, `stereo_hook=inline`). Code: `linux/bmvr_preload.c` (section
"stage 4b"), `linux/bmvr_stereo.h` (math), `linux/bmvr_rtti.h` (vtable finder), `linux/bmvr_inline.h` (inline detour).

## How it works

1. **Finding `RenderView` without offsets.** `client.so` is stripped but RTTI survives. The shim reads `/proc/self/maps`, finds the string
   `11CViewRender`, the `type_info` pointing at it and the vtable pointing at that (Itanium ABI), and can patch slot 6 with `mprotect`
   (RELRO page; protection restored exactly). If the class is not found it lists every `*ViewRender` class; if `CViewRender` has no RTTI
   vtable it uses another class only when exactly one usable one exists (never overriding `stereo_class=`).
2. **Inline detour.** On the real build the vtable slot was called **0 times in 2400 frames** (the engine does not reach `RenderView`
   through it). `stereo_hook=auto` therefore installs an inline detour after 300 silent frames; the installer config says `inline` directly.
   The entry bytes on this build are `55 89 e5 57 56 53 81 ec 3c 03 00 00 8b 45 08 e8` (`push ebp; mov ebp,esp; push edi; push esi; push
   ebx; sub esp,0x33c; mov eax,[ebp+8]; call ...`). The decoder steals whole instructions covering at least 5 bytes (here exactly 5) and
   refuses calls, jumps and unknown opcodes, logging the bytes.
3. **Two views.** For a *delivery* frame with a pose and a sane main view the detour copies the engine's `CViewSetup`, writes origin /
   angles / fov / aspect / off-centre fractions for the left eye, calls the original `RenderView` (without the HUD flag, as the Windows
   build does), blits the finished target into eye 0's shared image, repeats for the right eye, then publishes ONE pair. Every other call
   (not a delivery frame, nested water/monitor/cubemap views, views under 640x360, no pose yet, failed layout check, dead helper) goes
   straight to the game's own `RenderView`. Verified `CViewSetup` layout (same as Windows): fov 0xb4, origin 0xbc, angles 0xc8 (yaw at
   0xcc), aspect 0xe8, off-centre flag 0xec; checked on every view, not assumed.
4. **Pose to view** (`bmvr_stereo.h`, pinned by `stereo_test.c`): OpenXR (+X right, +Y up, -Z forward, metres) to Source (+X forward, +Y
   left, +Z up, `stereo_scale` units/m, default 39.37). The first pose after start (or after `touch <game>/bmvr_recenter`) is the
   reference: an eye sits at the game's own camera origin plus its offset from the reference head position, turned by (game yaw -
   reference yaw). Pitch and roll are the headset's, yaw is the game's plus the head's turn since the reference, so walking, crouching and
   mouse-turning stay the game's and room-scale motion and head rotation are the headset's. The frustum mapping (`fov`, `aspect`, four
   0..1 fractions) is `VR::EyeOffCenterView` from the Windows build. The engine target keeps its own size; the full-rect blit squashes it
   into the eye image (the Windows "squash blit").
5. **Pose id.** A pair delivered from the hook is stamped with the pose it was rendered with, never a newer one.
6. **Safety.** Every refusal is logged and leaves the game running: class not found or not exactly one primary vtable, slot not
   executable, mprotect refused, no pose, head-locked (VIEW) poses, `CViewSetup` out of range, eye capture failing 5 times in a row
   (stereo off). `bmvr_in_stereo.flag` is created before the first stereo frame and removed after 600 good frames or a clean exit; if it
   exists at start the previous launch died with stereo active and stereo is off for this launch only.

## Why `+mat_queue_mode 0` is required

Without it pairs stopped after 35 (`stereo pairs 35`, `RenderView calls` still rising). Working hypothesis, inferred and not read from a
log: queued rendering moved `RenderView` to a thread without the current GL context so `xfer_gl_usable()` returned 0. With
`+mat_queue_mode 0` rendering and head tracking worked in the headset. The status line now ends `silent exits: not armed / no GL context
/ small view / helper gone / blit failed`, and the first no-GL-context exit logs the thread id and the hint. `bmvr-bms.sh` appends the
argument automatically.

## Run history

- **Run 1** (vtable hook only): picture fixed to the headset, no depth; the game log was not part of the report. Led to the diagnostics
  below: first-call log, raw dump of the first three views, status line every 600 frames, WARNING when the slot is never called within
  1800 frames; pairs the hook did not draw carry pose id 0 and are shown by the helper as one flat head-locked picture (same orientation,
  same symmetric 80 degree FOV in both eyes). Seeing that flat picture means the hook is not drawing eyes; seeing depth means it is.
- **Run 2** (log read): RTTI found `11CViewRender` (one primary vtable at module offset 0xbe9c88, secondary 0xbe9d54); slot 6 is
  `RenderView` at module offset 0x714870 (slots 0-6 within 0x6e9100-0x714870, slot 7 at 0x3ac1b0, consistent with `IViewRender` where
  slot 7 is the trivial `GetDrawFlags`); called 0 times. The module also holds `20CBlackMesaViewRender` and `11IViewRender`; which class
  the engine really instantiates was not established, and the inline detour makes that irrelevant.
- **Inline detour** verified: called every frame from tick 362; view 3840x2160; left/right eye origins 2.4 units apart; per-eye frustum
  fractions from the pose channel.
- **User report 2026-11-22 12:23:** correct rendering and head tracking with `+mat_queue_mode 0`. That run's log was not read.
- A later run (3178 pairs, 0 dropped, no silent exits) confirmed the automatic `+mat_queue_mode 0` and `stereo_hook=inline` defaults.

## Try it / read the log

1. `scripts/build-linux.sh`, start SteamVR with the headset awake, launch the game (the helper starts itself).
2. In `bmvr_linux_log.txt` look for `stereo: RTTI 11CViewRender ... primary` and the slot dump, then `stereo: hooked ...`, then
   `stereo: frame 0 pose N: game view WxH fov ... -> left eye origin ...`. A `drawing the game's own view: ...` line says which check failed.
3. In the headset check depth on a nearby object, the direction of head turn and the picture size. `touch "$GAME/bmvr_recenter"` re-centres.

## Tests and what is still unverified

`stereo_test.c` (39 checks: axis map, angle conversion against `AngleVectors`, look-left = +yaw, look-up = negative pitch, room-scale
steps with a turned game, frustum round trip, bad input refused), `rtti_test.cpp` (27 checks against real g++ RTTI incl. a genuinely
read-only vtable page), `inline_test.c` (12 decoder checks), suite T24-T31 (fake `CViewRender`; two draws per delivery frame; eye origins
64 mm apart; HUD flag removed; `every=3`; refusals leave the game untouched; stale flag; helper death; wrong class name).

Not verified: the 32-bit inline patch code beyond the one real run (no 32-bit CI), why exactly pairs stopped without `mat_queue_mode 0`,
whether the target is complete at the end of `RenderView` (post-processing, viewmodel, resolve) for every scene, whether nested water views
inherit the eye setup, IPD and scale against a measurement, GPU cost of the second scene render. HUD, viewmodel hands and the weapon wheel
are not drawn in the eyes (see [hands.md](hands.md) and [roadmap.md](roadmap.md)). Frame pacing: the game delivers ~30 pairs/s against a
90 Hz compositor (`pairs=30/s displayed=90/s reused=60/s`); the Windows "one pair per `xrWaitFrame` pose" handshake is not ported.
