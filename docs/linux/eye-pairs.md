# Two-eye hand-off (stage 3d)

Status: transport implemented, stand-in tested (T14-T19) and verified in the real game (pairs delivered on an RTX 3080, kill loop
30/30 with `--eyes 2`, 32-bit suite passes). Stereo *content* arrived with stage 4b ([stereo-views.md](stereo-views.md)).

## Why a pair is one message and one GPU hand-off

Lesson from `docs/openxr-eye-publish.md` (defect 2, non-atomic left/right publish): two eyes taken from different game frames look
like ghosting while the head moves. So a pair is never two independent publications.

    GL side     : wait(vk_done) [not before the first pair] -> blit eye 0 -> blit eye 1 -> signal(gl_ready) -> flush -> frame message
    helper side : wait(gl_ready) -> read both images -> signal(vk_done)  (queue submit; ack is sent after the submit)

ONE semaphore pair covers both eyes, so the hang-risk analysis of 3a/3b is unchanged instead of doubled. Wire format:
[protocol.md](protocol.md).

## Shim

- Config: `target2=name:<label>|size:WxH`, `flip2=0|1|alt` (default = `flip`); env `BMVR_TARGET2`, `BMVR_FLIP2`. `target2=window` is
  refused. With `flip=alt` both eyes use the same orientation. With `stereo=1` no `target2` is needed ([stereo-views.md](stereo-views.md)).
- Capture state is per eye; one `SetRenderTarget` leave can fill both eyes (same object twice).
- A pair is delivered only when BOTH eyes were captured in the same armed frame. Otherwise nothing is sent and the log says which
  eye was missing. The loading/menu fallback applies per eye.
- Two imported textures, two draw FBOs, one read FBO; the semaphore barrier lists name both textures.
- Eye 1 is used only when requested AND the helper offers two images; every other combination logs the reason and delivers eye 0 only.

## Helper

- `--eyes 1|2` (default 1): two images, two readback buffers, ONE semaphore pair, one command buffer copying both.
- Snapshots of a pair: `bmvr_snapshot_<n>_eye0_<what>_flip<f>.ppm` and `..._eye1_...`.

## Tests

| Case | What it pins down |
|---|---|
| T14 | sem pair: `WAIT BLIT BLIT SIGNAL FLUSH frame ack`; one signal per pair; both textures in every barrier; two distinct blits; no `glFinish` |
| T14b | finish pair: `BLIT BLIT FINISH frame ack` |
| T14c | eye 1 chosen by size, own flip |
| T15 | eye 1 stops rendering: no frame at all afterwards (never eye 0 alone), the missing eye is named |
| T16 | negotiation: helper offers 2 / no target2; target2 / helper offers 1; target2 with an eye-0 window |
| T17 | helper exits right after acking a pair: no GL wait, blit or signal afterwards |
| T18 | other protocol versions and `eye_count` 3 rejected |
| T19 | loading/menu frames with two eyes |

## Real-game results (2026-11-20, RTX 3080, `sync=sem`, `target=InternalRT0`, helper `--eyes 2`)

- Same object for both eyes: 5 pairs (ticks 240-720), eye0/eye1 files written for each, 99.7-100% non-black, identical percentage
  for both eyes; the log reports both GL textures; no hang.
- Kill loop `--eyes 2`: 10/10, later 30/30 (helpers killed at random points, including between writing the eye0 and eye1 file).
- `target2=name:_rt_hud`: pairs delivered every time, but eye 1 was **exactly (0,0,0)** in all 5 snapshots. Most likely an empty HUD layer
  (menu and loading screens); not proven.
- `target2=name:_rt_gblightdiff`: the author reports the test completed and successful (eye 1 carried a real second target). The
  snapshot numbers were never recorded; that report is kept as given.
- Candidate second targets from the rt-trace (all 3840x2160): `_rt_hud`, `_rt_gblightdiff`, `_rt_gbdepthrt`.

The authoritative stand-in run is the plain 32-bit `tests/linux/run-tests.sh`; it passed on the author's machine (lock test,
T1-T19, real-helper section, selftest PASS on the RTX 3080).
