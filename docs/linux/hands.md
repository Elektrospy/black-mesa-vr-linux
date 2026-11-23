# Hand models, finger tracking, weapon viewmodel (stage 5, slices 5, 5b, 6)

Status: the CPU half is tested against the real files; **all GL drawing and the viewmodel substitution are unverified in the game and in
a headset.** If the log says `hands: ... disabled` or a hook refuses, the game is unaffected.

## Slice 5: hand models (`hands=1`)

The same ripped GLB hands as the Windows mod (`VR/hands/hev_glove_*_model.glb`, `bare_hand_*_model.glb`), drawn at the controllers with
fingers bent by trigger and grip.

- **`linux/bmvr_hands.h`** (no engine, no GL): inflate + PNG decoder (the GLB embeds its texture), a small JSON reader, the GLB loader
  (one skinned mesh), the finger rig and curl palette (same joint names, hinge limits and ValveBiped sign as `src/vr_hands.cpp
  BuildSummaryCurlPalette`), `bh_controller_world` (= `BuildControllerWorld`), an off-centre per-eye projection.
  `bmvr_stereo_pose_world` puts a controller in the world by the same rule as the eyes.
- **Drawing** (`bmvr_preload.c`, "hands"): after each eye's `RenderView`, before the blit to the helper, `hd_draw_eye` draws the hands with
  a small GL 3.2 shader (GPU skinning, 32 joint matrices) into the eye's target through a private FBO and restores every piece of GL state
  it touched. The helper receives eyes that already hold the hands.
- **Config:** `hands`, `hands_model`, `hands_right`, `hands_scale`, `hands_roll`, `hands_offset`, `hands_rot`, `hands_dir`, `hands_fingers`
  ([config.md](config.md)). **`hands_rot` defaults to `0,270,0` on Linux**: the Windows build's `0,180,0` left the glove 90 degrees about
  the palm normal toward the thumb on the Steam Frame controllers. `build-linux.sh` copies `VR/hands` into the game folder.
- **Limits:** drawn over the world (own depth buffer; a hand inside a wall still shows); a fixed headlight instead of the scene light.
- **Live tuning:** `hands_scale`, `hands_roll`, `hands_offset`, `hands_rot`, `hands_right`, `vm_offset` and `vm_rot` are re-read when
  `bmvr_target.txt` changes while the game runs (`config: applied '...'` in the log). No restart. Tune `hands_offset` / `hands_rot` /
  `hands_scale` first.
- **Not verified:** shader compile on the real driver, the FBO on the engine's target, restored state, hand placement and size, texture
  orientation, the flip rule `!xfer_pick_flip`. `hands: first hand drawn: ...` appears once the first draw ran.
- **Tests:** `hands_test.c` (67 checks: four GLBs load, PNG decodes, weights/joints sane, open curls give the bind pose, a fist moves
  fingertips not the palm, curls from trigger/grip, placement, projection, `pose_world`), T37 (no shader entry points: hands switch
  themselves off, pairs keep flowing).

## Slice 5b: finger tracking (protocol v7)

- Source: the helper is an OpenXR client, so SteamVR's skeletal input (Index finger sensors, Vive touch areas) arrives through
  `XR_EXT_hand_tracking` (26 joints per hand). No OpenVR library, no second session.
- **Helper** (`bmvr_xr_sink.c`): when the extension is offered (and `BMVR_NO_FINGERS` is unset) it is enabled at instance creation; after
  the actions are attached the helper checks `supportsHandTracking`, creates one tracker per hand and locates the joints once per frame.
  `bmvr_fingers.h` turns them into five curls with the Windows helper's formula (`ComputeCurl`: sum of a finger's joint bend angles / 3.2
  rad). A hand counts as tracked when the runtime says it is active and at least 4 of 5 fingers have all their joints.
- **Shim:** per hand and frame, tracked curls when the flag is set, otherwise `bh_curls_from_input` (analog trigger = index, grip =
  middle/ring/pinky, thumb a little; the Windows OpenXR fallback). `hands_fingers=off` forces trigger/grip. Log: `hands: the left hand's
  fingers now come from ...`; helper log: `fingers: ...`.
- **Fallback is automatic:** missing extension, unsupported system, failed tracker, inactive hand or too few joints just leave the flag
  clear. No errors, no noisy retries.
- **Not verified:** that SteamVR's OpenXR runtime reports controller-driven joints on your hardware, that the curls look right on the
  models (a fist may need tuning), the helper on a real runtime. `hands_test.c` checks the curl maths with synthetic joints.

## Slice 6: weapon viewmodel at the controller (`viewmodel=1`; stand-in tested only)

Added after the first hands work. The installer's default `bmvr_target.txt` now contains `viewmodel=1`.

- **Hook:** `C_BlackMesaViewModel::CalcViewModelView(owner, eyePosition, eyeAngles)` (`vm_class`, `vm_module`, `vm_slot`; default slot 231:
  Windows 230 plus the Linux vtable's extra destructor slot). It watches first and substitutes only after 30 consecutive calls with a
  plausible, smoothly moving eye position and angles (same scheme as `CreateMove`). A call that later stops looking right three times in
  a row switches it off for the launch. A crash leaves `bmvr_in_viewmodel.flag` and the next launch runs without it once.
- **Substitute eye** (`linux/bmvr_viewmodel.h`, `viewmodel_test.c`): angles = the weapon controller's AIM pose (+ `vm_rot=pitch,yaw,roll`);
  position = the GRIP pose position minus `vm_offset=fwd,right,up` (Source units, default 16,3,-2, the Windows defaults) along those
  angles. `vm_offset` is where the gun sits relative to the eye in the engine's own placement; if the gun floats in front of, behind or
  beside the hand, change it.
- **Slot search (`vm_probe=N`):** the first real-game log showed slot 231 was never called. With `vm_probe` the shim puts transparent
  probes (`linux/bmvr_probe.h`) on slots `vm_slot-N .. vm_slot+N`, watches which is called as `(this, owner, eye position, eye angles)` 30
  times in a row, removes the probes and hooks that slot for real. The probe is a thunk that never touches the stack and ends in a plain
  `jmp` to the original, so any function shape (struct or float returns, extra arguments) is unaffected; `probe_test.c` runs it as a real
  `-m32` freestanding program. Log: `probing found it: slot X` (then set `vm_slot=X vm_probe=0`), or every 600 frames `probe slot X: N
  calls, M plausible` for each probed slot that was called. 32-bit build only. The code default is `vm_probe=24` (an older note in the
  source docs said 8).
- **Not done:** the viewmodel FOV (the gun is still drawn with the engine's viewmodel projection), per-weapon offsets, re-applying the
  origin after the original runs. Which slot is really `CalcViewModelView` on the Linux build is **not yet established**.
- **Tests:** T38 (hook watches, then places the gun at the controller), T39 (wrong slot or class changes nothing and says why), T40 (slot
  guessed wrong: probes find it next door; 32-bit build only), `viewmodel_test.c`, `probe_test.c`.
