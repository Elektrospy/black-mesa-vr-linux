# Linux port documentation

Everything about running Black Mesa VR on Linux lives in this folder. The Windows mod is documented in the parent
`docs/` folder and is unchanged by any of this.

There are two separate things. Do not mix them up.

| | Black Mesa **through Proton** (Windows build) | Black Mesa **native Linux build** |
|---|---|---|
| What runs | the normal `d3d9.dll` + `OpenXRHelper64.exe` | the `linux/` port: a 32-bit `LD_PRELOAD` shim + a 64-bit OpenXR helper |
| Installer | `scripts/bmvr-linux.sh` | `scripts/build-linux.sh` |
| Status | reported working (PSVR2, SteamVR on Linux) | stereo and head tracking verified in a headset; **gameplay not verified** (details below) |
| Docs | [proton.md](proton.md) | everything else in this folder |

## Status of the native port (as of 2026-11-23)

Status words follow `AGENTS.md` rule 17. "Verified" always means observed on hardware or in the real game and written down
in [verification-log.md](verification-log.md); anything else is "stand-in tested" (mocks, no GPU) or "compiled".

| Area | Status | Doc |
|---|---|---|
| Shim loads in the real game, ToGL interposed, render targets listed | verified | [render-target-capture.md](render-target-capture.md) |
| 32-bit game -> 64-bit helper GPU hand-off (Vulkan memory imported into GL) | verified (RTX 3080) | [render-target-capture.md](render-target-capture.md) |
| GPU semaphore sync (`sync=sem`), helper-death guard, kill loop 30/30 | verified | [gpu-sync.md](gpu-sync.md) |
| One helper per game folder (`flock`) | stand-in tested; real-helper check still pending | [gpu-sync.md](gpu-sync.md) |
| Two-eye hand-off (a pair is one atomic publication) | verified (transport) | [eye-pairs.md](eye-pairs.md) |
| Native OpenXR sink in the helper (probe, self-test, game picture in the headset) | verified on SteamVR 2.19.4 + RTX 3080; Monado only with stand-ins | [openxr-helper.md](openxr-helper.md), [openxr-results.md](openxr-results.md) |
| Real stereo (two `RenderView`s) and head tracking | **verified in the headset 2026-11-22** (user report; needs `mat_queue_mode 0`, inline hook) | [stereo-views.md](stereo-views.md) |
| Controller state reaching the game (protocol v6/v7) | verified end to end on SteamVR (sticks); triggers, grip, bumper and face buttons unconfirmed | [input.md](input.md) |
| Controllers driving `CreateMove` (walk, turn, attack, menu, weapon switch) | compiled + stand-in tested; **not verified in the game** | [input.md](input.md) |
| Aim from the weapon controller (`input_aim=1`) | compiled + unit tested; not run in the game | [input.md](input.md) |
| Hand models (`hands=1`) and finger tracking | CPU half tested against the real files; GL drawing and fingers **not run on a GPU / headset** | [hands.md](hands.md) |
| Weapon viewmodel at the controller (`viewmodel=1`, slot probe) | stand-in tested; the first real log showed the guessed slot is never called, real slot not yet established | [hands.md](hands.md) |
| HUD in the world, viewmodel at the controller, weapon-wheel graphic | not implemented | [roadmap.md](roadmap.md) |
| Frame pacing (game ~30-60 pairs/s vs 90 Hz compositor) | open | [roadmap.md](roadmap.md) |

Gameplay in the headset (the acceptance criterion in `AGENTS.md` rule 18) has **not** been demonstrated on Linux.

## Reading order

New to the port:

1. [native-build.md](native-build.md): why the native build needs a different architecture than Windows.
2. [architecture.md](architecture.md): the pieces, the data flow, which source file does what.
3. [install-and-run.md](install-and-run.md): build, install, launch options, logs, troubleshooting.
4. [config.md](config.md): every `bmvr_target.txt` key, environment variable, helper flag and marker file.

Design and history, in the order the stages were built:

| Stage | Doc |
|---|---|
| 1, 2a, 2b: observe-only shim, GPU hand-off, render-target capture | [render-target-capture.md](render-target-capture.md) |
| 3a-3c: GPU semaphores, loading frames, helper-death guard, single helper | [gpu-sync.md](gpu-sync.md) |
| 3d: two-eye pairs | [eye-pairs.md](eye-pairs.md) |
| Wire format, versions 1-7 | [protocol.md](protocol.md) |
| 4a: OpenXR sink design | [openxr-helper.md](openxr-helper.md) |
| 4a: OpenXR bring-up, runtimes, X0-X4 results | [openxr-results.md](openxr-results.md) |
| 4b: stereo views and head tracking | [stereo-views.md](stereo-views.md) |
| 5: controllers, menu, aim | [input.md](input.md) |
| 5: hands, fingers, weapon viewmodel | [hands.md](hands.md) |

Cross-cutting:

- [testing.md](testing.md): the stand-in suite, unit tests, real-game tests.
- [verification-log.md](verification-log.md): dated real-hardware runs, with the evidence each one gave.
- [roadmap.md](roadmap.md): what is open and what comes next.

## What changed in the repository

Nothing in the Windows build. The only edit to existing files is a guarded `add_subdirectory(linux)` in the root
`CMakeLists.txt`, skipped whenever `WIN32` is set (that includes the clang-cl/xwin cross build, which sets
`CMAKE_SYSTEM_NAME=Windows`).

The old top-level files `docs/LINUX_PORT.md`, `docs/LINUX_OPENXR_CONSUMER.md` and `docs/LINUX_STEREO_HANDOFF.md` were
split into this folder; their content is distributed as listed above.

## Related files outside this folder

- `README.md` (root): short Linux quick start for the OpenXR runtime.
- `packaging/BMVR_README.txt`: the same for the release zip.
- `tests/linux/README.md`: file-by-file description of the stand-in tests.
- `tools/linux/`: probe and diagnose scripts for the native build.
- `docs/openxr-eye-publish.md`: the Windows OpenXR pipeline whose defects the Linux design avoids.
