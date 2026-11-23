# Roadmap and open items (native Linux port)

## Done

1. Observe-only shim, GPU hand-off, render-target capture ([render-target-capture.md](render-target-capture.md)).
2. GPU semaphores, loading-frame fallback, helper-death guard, one helper per folder ([gpu-sync.md](gpu-sync.md)).
3. Two-eye pairs ([eye-pairs.md](eye-pairs.md)).
4. Native OpenXR sink on SteamVR ([openxr-results.md](openxr-results.md)).
5. Stereo views with head tracking, inline `RenderView` detour, `mat_queue_mode 0` ([stereo-views.md](stereo-views.md)).
6. Controller channel to the game: protocol v6/v7, Steam Frame bindings, `CreateMove` hook, menu, weapon switching, aim, hands, fingers,
   viewmodel hook ([input.md](input.md), [hands.md](hands.md)); compiled and stand-in tested, not verified in the game.

## Next (in the order the notes planned them)

1. **Verify controller gameplay in the headset**: walking, turning, shooting, menu, weapon switch; confirm triggers, squeeze, bumper and face
   buttons actually arrive (still unconfirmed).
2. **Establish the real `CalcViewModelView` slot** (probe results from a real log) and place the weapon at the controller; then the
   viewmodel FOV and per-weapon offsets.
3. **Verify hands drawing and finger tracking** on a GPU and in a headset; tune `hands_*`.
4. **HUD in the world**, bloom/exposure on the eyes, weapon-wheel graphic with the player's real weapon list, menu pointing with a
   controller ray instead of the stick cursor.
5. **Frame pacing**: the game delivers ~30 pairs/s against a 90 Hz compositor; port the Windows "one pair per `xrWaitFrame` pose" handshake
   (`docs/openxr-eye-publish.md`).
6. Platform layer: shared code stops depending on `windows.h`, MinHook and SEH (the original plan item 6).

## Open verification items

- Helper `flock` against the real helper binary (two helpers, second must refuse).
- Helper auto-start in `bmvr-run.sh` end to end (only `bash -n` so far).
- Loading-screen fallback: that the back buffer holds the loading picture at the end of those frames.
- `sync=finish` soak after the queue fix; kill loop with `--sink xr` against the real game; GPU copy time at 3036x3036.
- Automatic comparison of the runtime's Vulkan device UUID with the shim's GL UUID (hybrid-GPU machines).
- Depth layer (`XR_KHR_composition_layer_depth`); poses going `stale` while the game is paused or loading.
- Why exactly pairs stopped without `mat_queue_mode 0` (hypothesis only); which of `CViewRender` / `CBlackMesaViewRender` the engine uses.
- 32-bit CI for the inline-patch and probe code.
- Monado: only stand-in runs so far; Flatpak Steam and the per-launch `XR_RUNTIME_JSON` override untested.
- `build-linux.sh --uninstall` does not remove `bmvr_helper_log.txt`, `bmvr_in_*.flag` or `VR/hands`; its epilogue text is partly stale.
- Proton: `WINEDLLOVERRIDES` and `steam_appid.txt` for `--separate` unverified.
