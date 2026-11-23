# Configuration reference (native Linux port)

## `bmvr_target.txt` (game folder)

One `key=value` per line. Read by the shim at start. The installer writes this default only when no file exists:

    target=InternalRT0
    flip=0
    sync=sem
    every=1
    stereo=1
    stereo_hook=inline
    input=1
    viewmodel=1

The shim's own code defaults differ (stereo, input and hands are opt-in; `every` is 120). The file the installer writes is what
turns the port on.

### Transport and capture

| Key | Values | Meaning |
|---|---|---|
| `target` | `name:<substring>` (default `name:_rt_fullframefb`), `size:WxH`, `window`, or a bare label such as `InternalRT0` | which render target is delivered as eye 0 ([render-target-capture.md](render-target-capture.md)) |
| `target2` | `name:<label>`, `size:WxH` | eye 1 source for the mono two-eye test; not needed with `stereo=1`; ignored when eye 0 is `window` |
| `flip`, `flip2` | `0`, `1`, `alt` | Y orientation of the blit. `alt` alternates per snapshot (discovery). `flip2` defaults to `flip` |
| `sync` | `finish` (default in code), `sem` | `glFinish` + ack, or GPU semaphores ([gpu-sync.md](gpu-sync.md)) |
| `every` | 1..3600 (code default 120) | one delivery per N presents. `1` is needed for a live picture |

### Stereo (stage 4b, [stereo-views.md](stereo-views.md))

| Key | Default | Meaning |
|---|---|---|
| `stereo` | off (installer: `1`) | draw two eye views from the helper's pose |
| `stereo_hook` | `auto` (installer: `inline`) | `auto` = vtable slot, then inline after 300 frames without a call; `vtable`; `inline` |
| `stereo_module` | `/client.so` | module holding the view class (`exe` = main program) |
| `stereo_class` | `CViewRender` | class whose vtable holds `RenderView`; never substituted when given |
| `stereo_slot` | 6 | vtable slot of `RenderView` |
| `stereo_scale` | 39.37 | Source units per metre |

### Input (stage 5, [input.md](input.md))

| Key | Default | Meaning |
|---|---|---|
| `input` | off (installer: `1`) | controllers drive `CreateMove` |
| `input_module`, `input_class`, `input_slot`, `input_probe` | `/client.so`, `ClientModeShared`, 22, `1` | where `CreateMove` is hooked; `input_probe=0` stops watching subclass vtables |
| `input_weapon_hand` | `right` | `left` mirrors both controllers |
| `input_move_hand` | opposite of the weapon hand | `left` or `right` (equal to the weapon hand: one hand walks and fires) |
| `input_deadzone` | 0.15 | 0.05..0.5 |
| `input_trigger` | 0.55 | press point, 0.2..0.9 |
| `input_turn` | `smooth` | `smooth` or `snap` |
| `input_turn_speed` | 180 | degrees/s at full deflection, 10..720 |
| `input_turn_analog` | 1 | `0` = one constant speed past the deadzone |
| `input_snap` | 30 with `input_turn=snap` | degrees per flick, 0 or 5..120 |
| `input_btn_a`, `_b`, `_bumper`, `_x`, `_y` | reload, attack2, jump, duck, none | each one of `none reload jump duck attack2 use attack` |
| `input_aim` | `0` | `1`: shots follow the weapon controller (needs `stereo=1`) |

### Hands (stage 5, [hands.md](hands.md))

| Key | Default | Meaning |
|---|---|---|
| `hands` | off | draw hand models at the controllers |
| `hands_model` | `hev` | `hev` or `bare` |
| `hands_right` | `0` | also draw the weapon hand |
| `hands_scale` | 0.85 | 0.2..3 |
| `hands_roll` | 90 | degrees about the controller's forward axis, mirrored per hand |
| `hands_offset` | `0,-0.008,-0.10` | metres |
| `hands_rot` | `0,270,0` | degrees (Windows uses `0,180,0`; on Steam Frame controllers that is 90 degrees off) |
| `hands_dir` | `<game folder>/VR/hands` | where the `.glb` files are |
| `hands_fingers` | auto | `off` forces trigger/grip curls |

### Weapon viewmodel ([hands.md](hands.md))

| Key | Default | Meaning |
|---|---|---|
| `viewmodel` | off (installer: `1`) | place the gun at the weapon controller by hooking `CalcViewModelView` |
| `vm_module`, `vm_class` | `/client.so`, `C_BlackMesaViewModel` | where the hook is looked for |
| `vm_slot` | 231 | vtable slot (Windows 230 + 1); unverified on Linux |
| `vm_probe` | 24 | search slots `vm_slot-N .. vm_slot+N` with transparent probes; `0` trusts `vm_slot` only |
| `vm_offset` | `16,3,-2` | forward,right,up in Source units: where the gun sits relative to the eye (live) |
| `vm_rot` | `0,0,0` | pitch,yaw,roll degrees added to the aim pose (live) |

Live keys (`hands_scale`, `hands_roll`, `hands_offset`, `hands_rot`, `hands_right`, `vm_offset`, `vm_rot`) are re-read when `bmvr_target.txt`
changes while the game runs; the log prints `config: applied '...'`.

## Environment variables

Shim: `BMVR_TARGET`, `BMVR_TARGET2`, `BMVR_FLIP`, `BMVR_FLIP2`, `BMVR_SYNC`, `BMVR_STEREO`, `BMVR_INPUT` (each overrides the file key
of the same name).

Launcher: `BMVR_NO_HELPER=1` (do not start the helper), `BMVR_HELPER_ARGS="..."` (replace the helper's arguments),
`BMVR_KEEP_QUEUE=1` (do not append `+mat_queue_mode 0`), `BMVR_REAL_BMS` (path of the original start script).

Helper: `BMVR_NO_FINGERS` (no hand tracking), `BMVR_XR_ONE_QUEUE=1` (single queue + mutex, A/B comparison only),
`XR_RUNTIME_JSON` (pick the OpenXR runtime for this process).

Build/test: `BMVR_GAME_DIR`, `BMVR_TEST_NATIVE=1`, `BMVR_TEST_BUILD`.

## Helper command line (`bmvr_xfer_helper [game folder] ...`)

| Option | Meaning |
|---|---|
| `--sink snapshot\|xr` | snapshot PPM files (default, test sink) or the OpenXR sink |
| `--eyes 1\|2` | images offered to the shim (default 1; the launcher passes 2) |
| `--xr-layer quad\|projection\|both` | quad = flat screen; projection = per-eye views with poses (needed for stereo and poses) |
| `--xr-quad world\|head` | quad fixed in the room (default) or following the head |
| `--xr-size WxH`, `--xr-wait MS`, `--xr-check N` | swapchain size override, wait for READY (default 10 s), debug readback of N pairs |
| `--no-semaphores` | force the `glFinish` path from the helper side |
| `--size WxH`, `--gpu N`, `--snapshots N` | snapshot sink only |
| `--xr-probe` | X0: open the runtime, report, exit 0 PASS / 1 FAILED / 2 UNAVAILABLE |
| `--selftest-xr [--xr-layer ...] [--xr-seconds N]` | X1: generated pattern, left BLUE "L", right RED "R" |
| `--selftest-sync` | stage 3a: GPU semaphore export/import and 50 frame cycles, no game |

`--sink xr` never falls back to snapshots: if the runtime, headset or session is unavailable it exits with the reason (exit 2
unavailable, 1 failed).

## Marker and flag files (game folder)

| File | Meaning |
|---|---|
| `bmvr_xfer.sock`, `bmvr_xfer.lock` | helper's socket and its exclusive `flock` |
| `bmvr_in_stereo.flag` | created before the first stereo frame, removed after 600 good frames or a clean exit. If present at start the previous launch died with stereo active, and stereo is off for this launch only |
| `bmvr_in_input.flag`, `bmvr_in_viewmodel.flag` | same rule for controller input / the viewmodel hook |
| `bmvr_recenter` | `touch` it to re-centre on the current head pose |
| `bmvr_run_marker.txt`, `bmvr_inner_marker.txt` | proof that the host wrapper / container side ran |
