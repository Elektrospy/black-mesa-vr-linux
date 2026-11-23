# Architecture of the native Linux port

```
 32-bit game (bms_linux, ToGL = D3D9 on OpenGL)             64-bit helper (bmvr_xfer_helper)          OpenXR runtime
 ------------------------------------------------           -----------------------------------       ---------------
 LD_PRELOAD shim  bmvr_preload32.so                          Vulkan device owned by the runtime        SteamVR / Monado
   - interposes ToGL (Present, SetRenderTarget, ...)           (XR_KHR_vulkan_enable2)
   - hooks RenderView (inline detour): draws 2 eye views     exportable images + semaphores
   - hooks CreateMove: controllers drive the game  <--acks--   pair ring (3 slots) -> XR thread  ---->  xrEndFrame
   - draws hand models into the eye targets                    poses (xrLocateViews) + input actions
   - blits eye targets into imported GL textures  --frames-->  (OpenXR actions, hand tracking)
        shared GPU memory (opaque fd, SCM_RIGHTS over a unix socket) + GL_EXT_semaphore_fd / VK_KHR_external_semaphore_fd
```

One unix socket `<game folder>/bmvr_xfer.sock`; the helper listens, the shim connects and reconnects. The game never blocks for
long and never depends on the helper: if the helper dies the game runs on with its own view.

## Where the code lives

| File | Role |
|---|---|
| `linux/bmvr_preload.c` | the shim: ToGL interposition, render-target registry, GL import/blit, stereo hook, input hook, viewmodel hook, hands drawing, live config |
| `linux/bmvr_xfer.h` | wire format (protocol v7), shared by shim and helper |
| `linux/bmvr_xfer_helper.c` | the 64-bit helper: socket transport, snapshot sink, self-tests, command line |
| `linux/bmvr_xr_sink.c` / `.h` | OpenXR in the helper: probe (X0), self-test (X1), socket sink with ring and XR thread (X2-X4), input actions, finger tracking |
| `linux/bmvr_pair_ring.h` | 3-slot pair ring state machine (pure bookkeeping, unit tested) |
| `linux/bmvr_pose_store.h` | last 32 poses by id, stale counter (unit tested) |
| `linux/bmvr_single_instance.h` | one helper per game folder (`flock`) |
| `linux/bmvr_stereo.h` | OpenXR -> Source view math (pinned by `stereo_test.c`) |
| `linux/bmvr_rtti.h` | finds vtables of a stripped module through RTTI and patches a slot |
| `linux/bmvr_inline.h` | i386 inline detour (instruction-length decoder, trampoline) |
| `linux/bmvr_input_map.h` | controller state -> `CUserCmd` values, aim math, button map |
| `linux/bmvr_hands.h` | GLB loader, PNG/inflate, finger rig and curl palette, controller placement |
| `linux/bmvr_viewmodel.h` | substitute eye that puts the viewmodel on the weapon controller (pure math) |
| `linux/bmvr_probe.h` | transparent 32-bit call probe used to find an unknown vtable slot |
| `linux/bmvr_fingers.h` | OpenXR hand joints -> five curls |
| `scripts/build-linux.sh` | build + install (launcher files, helper, default config, hand models) |
| `scripts/bmvr-linux.sh` | the unrelated Proton installer, see [proton.md](proton.md) |

## One frame, end to end (stereo on)

1. The helper's XR thread locates both eyes (`xrLocateViews`) and keeps the pose by id. The newest pose rides back to the shim
   on every ack (protocol v5+).
2. The engine calls `CViewRender::RenderView`. The inline detour sees a top-level view on a delivery frame and, with a pose and
   a sane view, copies the engine's `CViewSetup`, writes the left-eye origin, angles, FOV and off-centre fractions, calls the
   original, blits the target into eye 0's shared image, repeats for the right eye. See [stereo-views.md](stereo-views.md).
3. If `hands=1`, the hand models are drawn into each eye's target before its blit ([hands.md](hands.md)).
4. Both eyes are published as one pair: one GPU wait, two blits, one signal, one frame message stamped with the pose id the pair
   was rendered with. See [eye-pairs.md](eye-pairs.md) and [gpu-sync.md](gpu-sync.md).
5. The helper copies the pair into a ring slot on the GPU and acks. The XR thread displays the newest complete slot on a
   projection layer using exactly that pose, reusing the previous slot when no new pair arrived. See
   [openxr-helper.md](openxr-helper.md).
6. The ack also carries the controllers' state. In `CreateMove` the shim turns it into walk, turn, buttons and (optionally)
   aim. With `viewmodel=1` the `CalcViewModelView` hook places the gun at the controller ([hands.md](hands.md)).

## Design rules that carry over from the Windows port

- A pair is never split (`docs/openxr-eye-publish.md`, defect 2).
- The pose submitted with a frame is the pose it was **rendered** with, and it travels with the frame.
- The submitted FOV matches the rendered frustum (the off-centre setup is `VR::EyeOffCenterView` from the Windows build).
- The game and the compositor run on separate clocks decoupled by a ring; the newest complete pair wins.
- Every guessed hook is verified at run time before anything is written, and a crash-sticky flag (`bmvr_in_*.flag`) disables
  only the attempt that killed the previous launch (`AGENTS.md` rule 20).
