# Verification log: dated real-hardware runs

Everything here was observed on the author's/user's hardware unless marked "report". Stand-in results live in the stage docs.
Hardware: NVIDIA RTX 3080, driver 620.70.06 (620.73.04 open kernel module later), CachyOS, Steam Linux Runtime "soldier", SteamVR 2.19.4,
headset via vrlink / Steam Frame.

| Date | What | Result | Where |
|---|---|---|---|
| (stage 1) | shim in the real game | interposition works, targets logged, GL extensions present | [render-target-capture.md](render-target-capture.md) |
| (stage 2a) | 64-bit helper imports the game's GPU image | UUIDs equal, snapshot is the game picture | same |
| (stage 2b) | `InternalRT0` through the pipe in gameplay | works; loading frames need 3b | same |
| (stage 3a) | `sync=finish` vs `sync=sem` | sem: 79-112 us vs 2.0-4.4 ms in the render thread | [gpu-sync.md](gpu-sync.md) |
| 2026-11-19 | kill loop, one eye | 10/10 | same |
| 2026-11-20 | two-eye pairs; kill loop `--eyes 2` | pairs delivered; 10/10 then 30/30; `_rt_hud` all black, `_rt_gblightdiff` reported OK | [eye-pairs.md](eye-pairs.md) |
| 2026-11-20 | X0 probe, X1 self-test on SteamVR | PASS; blue "L" / red "R" seen | [openxr-results.md](openxr-results.md) |
| 2026-11-21 | X2 with the real game | one queue failed (Xid 32/69, watchdog); own queue: 89-90 XR frames/s, lock wait 0 | same |
| 2026-11-21 | pose channel on SteamVR | poses reach the shim | same |
| 2026-11-22 | stereo hook run 1 and 2 | vtable slot never called; inline detour installed, eyes drawn | [stereo-views.md](stereo-views.md) |
| 2026-11-22 12:23 | `+mat_queue_mode 0` (user report) | correct rendering and head tracking in the headset | same |
| 2026-11-22 | input channel, run 1 and 2 | sticks arrive; Steam Frame profile bound (18 of 20 paths); face buttons, triggers unconfirmed | [input.md](input.md) |
| after 2026-11-22 | `vm_slot=231` real log | slot never called; probe search added | [hands.md](hands.md) |
| 2026-11-24 | viewmodel probe, real log (CachyOS, SteamVR) | probe "found" slot 244, which is a bone function `(this, ptr, bone pos, bone quaternion)`, not `CalcViewModelView`; the previous launch died with that hook live; the retry then read the probe's own `bmvr_in_viewmodel.flag` as a crash and switched the viewmodel off. Real slot still unknown (231 never called). Probe now requires the eye position to be near the game's view origin and logs which argument carries it; the flag no longer trips itself | [hands.md](hands.md) |
| 2026-11-25 | viewmodel direct placement, real logs | no probed slot (207..255) is `CalcViewModelView`, so the entity (`this` from the transparent probes) is written directly. Fixed Windows offsets crashed the game (+0x294 held other data); the origin is at +0x284 on Linux and the entity has a second eye-like origin at +0x254, so fields are now found at run time and every origin/angles pair is written. Not yet confirmed in a headset | [hands.md](hands.md) |

Not yet run in a game or headset: `CreateMove` control, aim, hands drawing, fingers, viewmodel substitution, helper auto-start, flock against
the real helper.
