# Build, install and run (native Linux build)

Native build only. For Proton see [proton.md](proton.md).

## Prerequisites

- Black Mesa installed as the **native Linux** version (the folder contains `bms_linux`).
- Build: cmake >= 3.20, a gcc that can produce 32-bit code (`gcc -m32` must work; Ubuntu: `gcc-multilib g++-multilib`).
- The 64-bit helper additionally needs the Vulkan headers (Arch/CachyOS: `vulkan-headers`; Debian/Ubuntu: `libvulkan-dev`). The
  OpenXR headers are vendored in `third_party/openxr`; the OpenXR and Vulkan loaders are `dlopen`ed at run time.
- An OpenXR runtime for the headset path: SteamVR as the **active** runtime (see [openxr-results.md](openxr-results.md)), or
  Monado.

## Build and install

    scripts/build-linux.sh --auto                                  # finds the native game in your Steam libraries
    scripts/build-linux.sh "/path/to/steamapps/common/Black Mesa"  # or pass the folder (or set BMVR_GAME_DIR)
    scripts/build-linux.sh                                         # build only

It always builds from scratch, checks that the shim exports the exact ToGL mangled names, builds the helper, runs the helper's
`--selftest-sync`, reports the highest glibc symbol version the shim needs, installs, and runs a launch self-test in a folder
with a space in its name. VS Code: `.vscode/tasks.json` has build / build+install (auto) / build+install (choose folder).

Then set the Steam launch option (keep the quotes; the folder name has a space):

    "<game folder>/bmvr-run.sh" %command%

Game arguments, if any, go **after** `%command%`. Anything before it becomes an argument of `bmvr-run.sh`, which runs it, and the
game does not start.

Do **not** put `LD_PRELOAD=...` in the launch options: Steam's Linux Runtime container (pressure-vessel) discards it.

## What the installer puts in the game folder

All files are new; the game's own `bms.sh` is never modified.

| File | Purpose |
|---|---|
| `bin/bmvr_preload32.so` | the shim |
| `bmvr-run.sh` | HOST-side wrapper (see below); also starts the helper |
| `bmvr-bms.sh` | CONTAINER-side starter: sets `LD_PRELOAD`, appends `+mat_queue_mode 0`, runs the original `bms.sh` |
| `bmvr_xfer_helper` | the 64-bit helper |
| `bmvr_target.txt` | default config, written only when none exists (see [config.md](config.md)) |
| `VR/hands/*.glb` | hand models, copied from the repository's `VR/hands` |

Written at run time: `bmvr_linux_log.txt` (shim), `bmvr_helper_log.txt` (helper, when started by `bmvr-run.sh`),
`bmvr_run_marker.txt`, `bmvr_inner_marker.txt`, `bmvr_xfer.sock`, `bmvr_xfer.lock`, `bmvr_in_stereo.flag` / `bmvr_in_input.flag` / `bmvr_in_viewmodel.flag`
while those features are active, snapshots `bmvr_snapshot_*.ppm` when the snapshot sink is used.

### Why two launcher files

Steam expands `"<wrapper>" %command%` to
`<wrapper> steam-launch-wrapper -- reaper ... -- scout-on-soldier-entry-point-v2 -- <game>/bms.sh ...`. The wrapper therefore runs
on the HOST, outside the Steam Linux Runtime container, and pressure-vessel rebuilds `LD_PRELOAD` inside it, dropping what was set
outside (observed: the game process had only Steam's overlay entries). So `bmvr-run.sh` swaps the `bms.sh` argument for
`bmvr-bms.sh`, a new file that runs inside the container, sets `LD_PRELOAD` and `LD_LIBRARY_PATH`, and runs the original
`bms.sh`. `LD_PRELOAD` holds the bare name `bmvr_preload32.so`, not a path, because it splits on spaces. The shim avoids glibc
symbols newer than the 2.34 `dlopen`/`dlsym` pair (pinned with `.symver` on i386) so an older runtime can load it.

### Helper auto-start

`bmvr-run.sh` starts `bmvr_xfer_helper --sink xr --eyes 2 --xr-layer projection` in the background (override with
`BMVR_HELPER_ARGS`, skip with `BMVR_NO_HELPER=1`), logs to `bmvr_helper_log.txt`, waits 2 s and keeps the helper only if it is
still alive (a second helper exits at once on the `flock`). It no longer `exec`s the game: it waits for it, returns its exit status
and stops the helper (SIGINT, then TERM, then KILL) only if this script started it. SteamVR must already be running with the
headset awake; otherwise the helper exits and the game starts with mono pairs. Status: written as a script and checked with
`bash -n`; **not run end to end**.

### `+mat_queue_mode 0`

`bmvr-bms.sh` appends it automatically (opt out with `BMVR_KEEP_QUEUE=1`). Stereo needs the game to render on the thread that
owns the GL context; with queued rendering `RenderView` moved to another thread and pairs stopped after ~35 frames (see
[stereo-views.md](stereo-views.md)). The Windows build also keeps `mat_queue` off.

## Uninstall

    scripts/build-linux.sh --uninstall [folder]

Deletes the files listed in the script (shim, launchers, markers, log, helper, socket, lock, `bmvr_target.txt`, snapshots).
Then clear the Steam launch option. Known gap: the script does not remove `bmvr_helper_log.txt`, the `bmvr_in_*.flag` files or
`VR/hands`; delete those by hand if you care.

## Logs and troubleshooting

| Symptom | Look at |
|---|---|
| No `bmvr_linux_log.txt` | `bmvr_run_marker.txt` (did the host wrapper run, did the swap happen?) then `bmvr_inner_marker.txt` (container side). Then `tools/linux/bmvr-diagnose-linux.sh`, ideally while the game runs |
| Game does not start | game arguments placed before `%command%` (`bmvr_run_marker.txt` shows `args: +mat_queue_mode 0 ...`) |
| Helper exits at once | SteamVR not running, not the active OpenXR runtime, or the headset in standby. `bmvr_helper_log.txt` says which. See [openxr-results.md](openxr-results.md) |
| Picture in the headset is flat and head-locked | the stereo hook is not drawing eyes: read the `stereo:` lines in `bmvr_linux_log.txt` ([stereo-views.md](stereo-views.md)) |
| Pairs stop after a few dozen frames | queued rendering is on (`mat_queue_mode`); the status line ends with `silent exits: ... no GL context ...` |
| Stereo or input turned itself off for one launch | a `bmvr_in_*.flag` was left by a launch that died; the log says so, the next launch tries again |
| `kill -9` of the helper hangs the game | should not happen (kill loop 30/30); report with the shim log |

Probe scripts: `tools/linux/bmvr-probe-linux.sh`, `bmvr-probe2-linux.sh` (what the native build links and exports),
`bmvr-diagnose-linux.sh` (why no log appears). Standard first question for any report: attach `bmvr_linux_log.txt` and
`bmvr_helper_log.txt`.

## Notes on the printed installer epilogue

`scripts/build-linux.sh` prints a long help text at the end. Parts of it predate stage 4b/5 (it still says "The shim only
observes" and that `--sink xr` is "untested with the real game"). This folder is the maintained description.
