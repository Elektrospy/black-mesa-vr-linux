# Controller input, game menu, aim (stage 5, slices 1-4)

Hardware in use: Steam Frame headset and controllers via SteamVR/OpenXR. Control scheme chosen by the user (2026-11-22): smooth
turning, left stick moves, weapon in the right hand; configurable ([config.md](config.md)).

Status: the channel (helper -> shim) is verified end to end; the `CreateMove` hook, menu control, weapon switching and aim are compiled
and stand-in tested but **not verified in the game**.

## Slices 1-2: the input channel (protocol v6)

- **Protocol v6** ([protocol.md](protocol.md)): every ack carries `struct bmvr_xfer_input`: per hand stick, trigger, squeeze, five
  buttons, aim and grip poses in the same LOCAL space as the eye poses, flags (focused, poses valid per hand).
- **Helper** (`bmvr_xr_sink.c`, `sk_input_*`): one action set; enables `XR_VALVE_frame_controller_interaction` when offered and suggests
  `/interaction_profiles/valve/frame_controller_valve` bindings (paths from Valve's Steamworks Steam Frame input page) plus
  `khr/simple_controller` as a fallback. `xrSyncActions` runs once per frame after `xrBeginFrame`; values are zero when the session is
  not focused. The log says which profile the runtime bound and whether the Valve bindings were accepted.
  Every binding is probed on its own; each bad one logs `Steam Frame profile rejects <path>` and the rest are suggested.
- **Shim** (`in_note`): logs the first state and every coarse change (`input: ...`, at most 200) and exports `bmvr_get_input()`.

**First headset run (2026-11-22, SteamVR 2.19.4):** channel works end to end (both poses valid, aim at about (-0.00 -0.31 -0.45) m in
LOCAL space), stereo ran 3178 pairs with 0 dropped. But SteamVR answered `XR_ERROR_PATH_UNSUPPORTED` to the whole Steam Frame suggestion,
so `khr/simple_controller` was bound (select/menu only). Fix: per-binding probing.

**Second run:** `/interaction_profiles/valve/frame_controller_valve` bound (18 of 20 accepted); both sticks arrive and change in the shim
log. Rejected: `/user/hand/left/input/x/click` and `y/click` (x/y live on the RIGHT hand with a/b; the left hand has the d-pad and
`view`). Remapped (compiled, not run): left primary/secondary = `dpad_down` / `dpad_up`; right x/y = `BMVR_XFER_BTN_EXTRA1/2`. No
trigger/squeeze/button change lines were in that log, so **triggers, squeeze, bumper and the face buttons are still unconfirmed.**

To verify: start SteamVR, launch, press buttons and push both sticks, send `bmvr_helper_log.txt` and `bmvr_linux_log.txt`. Expected:
`input: Steam Frame controller bindings suggested`, `input: controller actions attached`, `input: the runtime bound the right hand to
/interaction_profiles/valve/frame_controller_valve`, then `input:` lines when you press things.

## Slice 3: the `CreateMove` hook (`input=1`)

Hook: `IClientMode::CreateMove(float flInputSampleTime, CUserCmd *cmd) -> bool`, found through RTTI of `ClientModeShared` in
`/client.so`, vtable slot 22 (21 on the Windows build; the Itanium vtable has two destructor slots). Wrong guesses are caught at run
time: for `IC_LEARN` consecutive calls the hook only looks at the second argument, which must be readable memory laid out like a
`CUserCmd` with a plausible, increasing command number. Only then does it start writing; if the slot later stops looking like
`CreateMove` input turns itself off for the launch. `input_probe=1` also watches the vtables of `ClientModeShared` subclasses (T36: the
game may instantiate a subclass with its own vtable copy). A crash leaves `bmvr_in_input.flag`; the next launch runs without input once.
If the class is wrong the log lists the `ClientMode*` classes to pick from with `input_class=`; a slot past the vtable is refused.

What the controllers do:

| Input | Action |
|---|---|
| walk stick (opposite the weapon hand) | forward / side move, rotated into the cmd's yaw frame |
| other stick, horizontal | smooth turn (`input_turn_speed`, analog by default) or snap turn |
| trigger | attack; button map: `a` reload, `b` attack2, bumper jump, `x` duck, `y` none by default |
| left d-pad down / up | previous / next weapon (mouse-wheel event, Black Mesa default bindings assumed) |
| right stick click, hold + push + release | minimal weapon wheel: direction picks slot 1-6 (6 sectors clockwise from the top), sends the number key; no graphic, no inventory check |
| Menu / view button | Escape; `bmvr_recenter` is a file touch |
| flashlight, recenter | handled in the hook |

**Game menu.** While the game has stopped calling `CreateMove` for 6 Presents (pause, main menu, loading) the controllers drive the menu
through SDL's event queue: left stick / d-pad = arrow keys with repeat, A = Enter, B = Escape, right stick = mouse cursor
(`SDL_WarpMouseInWindow`), either trigger = left click. Log: `input: game menu control ON/off`. Not verified: that the old game UI takes
the injected events or that the cursor is visible in the headset.

## Slice 4: aim from the weapon controller (opt-in `input_aim=1`)

- With `input_aim=1` the cmd's `viewangles` carry the controller's aim so the engine fires along it (the shot still starts at the head,
  so there is parallax at close range). The aim is the controller `aim` pose rotated by `body_yaw - ref_yaw`, the same turn the eye views
  use (`bmvr_im_aim_angles`). It needs the stereo rig's head reference; without one the cmd keeps the body yaw.
- The game's view yaw is now the AIM yaw, so the body yaw (what turning changes, what the camera is built on) is tracked in the shim
  (`g_ic_body_yaw`) and the stereo rig takes it as the game yaw. An engine yaw change we did not make (mouse, small push) is added to the
  body yaw; a jump of more than 25 degrees (spawn, teleport, level change) resets it.
- Walking is rotated into the cmd's yaw frame. `input_weapon_hand=left` aims with the left controller.
- **Not verified in the game.** Risks: view angles other than the cmd's drive something else (viewmodel, crosshair); the engine may clamp
  or smooth the view; the 25 degree reset may misfire on a fast flick. Remove `input_aim=1` to return to the default path.
- The weapon is placed at the controller by `viewmodel=1`, see [hands.md](hands.md).

## Tests

`input_map_test.c` (59 checks incl. hand mirror, aim math: identity, 90 degrees left, nose up 45, body turn, zero quaternion, clamp, flag
swap), suite T23b/T23c (`mock_helper --input`), T32-T36 (hook watches first, then drives; no input offered; junk cmd pointer never
written; refusals say why; subclass vtable).
