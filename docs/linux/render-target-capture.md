# Observe-only shim, GPU hand-off, render-target capture (stages 1, 2a, 2b)

Code: `linux/bmvr_preload.c` (shim), `linux/bmvr_xfer_helper.c` (helper). Status: all three stages verified in the real game
(NVIDIA RTX 3080, driver 620.70.06, CachyOS, Steam Linux Runtime "soldier").

## Stage 1: the observe-only shim

A 32-bit `LD_PRELOAD` shim that forwards every ToGL call unchanged and logs `bmvr_linux_log.txt` in the game folder:

1. proof that the interposition works in the real game;
2. every render target / large texture the engine creates (size, format, label);
3. once per session a GL report: vendor, renderer, version, and whether `GL_EXT_memory_object_fd` / `GL_EXT_semaphore_fd` exist
   (needed for the Vulkan -> GL hand-off);
4. a frame-rate / call-count line every 5 s.

Verified in the real game: the launcher chain puts the shim into `bms_linux`; `CreateDevice` / `Present` / `CreateRenderTarget`
are interposed and forwarded; the render-target list is logged (3840x2160 `_rt_fullframefb`, `_rt_fullscreen`, `_rt_hud`,
`_rt_gbdepth`, CSM shadow targets, ...); the GL report shows both extensions. Verified with stand-ins: the root CMake patch applies
cleanly, a native configure produces a 32-bit ELF exporting exactly the ToGL names, a Windows-style configure still reaches the
original `Black Mesa is Win32` check, a 64-bit native configure fails with a clear message; forwarding of arguments and return
values through the PLT against a stand-in `libtogl.so`.

## Stage 2a: can a 64-bit process share GPU memory with the 32-bit game?

Required because the OpenXR runtime is 64-bit and the game is 32-bit.

    64-bit helper                                32-bit game (shim)
    Vulkan image, exportable memory   --fd-->    glImportMemoryFdEXT -> GL texture
    (VK_KHR_external_memory_fd)       (unix      glBlitFramebuffer(source -> texture)
    reads the image back, writes      socket,    every N presents, then glFinish
    bmvr_snapshot_N.ppm               SCM_RIGHTS) and waits for the helper's ack

Design notes:

- The shim does nothing unless `<game>/bmvr_xfer.sock` exists. If the helper dies the shim logs it, deletes its GL objects and goes
  back to looking. It never blocks longer than 2 s (socket timeouts), uses `MSG_NOSIGNAL`, and saves/restores the GL state it
  touches (FBO bindings, texture binding, scissor enable).
- The image is kept in `VK_IMAGE_LAYOUT_GENERAL` (an `UNDEFINED` old layout may discard contents).
- The helper prints its Vulkan device UUID; the shim logs `GL device UUID`. They must match (hybrid-GPU laptops).
- Synchronisation in 2a is `glFinish` plus an ack message; stage 3a replaces it ([gpu-sync.md](gpu-sync.md)).

**Result (RTX 3080):** Vulkan and GL UUIDs identical, image exportable (`exportable=1`, `dedicated-only=0`), imported by the shim,
`bmvr_snapshot_0.ppm` is the game picture (upright, 3840x2160 scaled to 1920x1080, 99.7-100% non-black in all five snapshots).
Caveats seen: the window's default framebuffer contained MangoHud and Steam overlay drawing, and because the read happens before
ToGL's own `Present` it may be the previous frame. Neither matters for engine render targets.

## Stage 2b: an engine render target instead of the window

Can the shim find the GL object behind a Direct3D render target without ToGL's private structs? Protocol bumped to v2 (frame
message carries the target label, GL object name and flip flag).

- **Registry.** `CreateTexture` (render-target usage), `CreateRenderTarget` and `IDirect3DTexture9::GetSurfaceLevel` (level 0)
  are interposed and record label + size per D3D object. `GetSurfaceLevel` is exported and called (~130 times per frame). ToGL
  hands out a fresh surface object on nearly every call, so the registry is two fixed tables: **pinned** (render-target textures
  and `CreateRenderTarget` surfaces, displaced only by other pinned entries, oldest first) and **transient** (the
  `GetSurfaceLevel` surfaces, overwrite-at-will). One shared table lost long-lived labels within seconds (regression test T11).
- **Resolving the GL object.** ToGL binds its framebuffer lazily, at draw time. When the engine calls `SetRenderTarget(0, X)`, the
  GL *draw* framebuffer still holds the target being *left*. The shim asks GL what is attached (`glGetFramebufferAttachmentParameteriv`),
  measures it and accepts it only if the size equals the registry size of the target being left. Multisampled targets are refused.
- **Final content of the frame.** The last accepted leave in the armed frame (the frame before each delivery) is remembered as
  (type, GL name, size); at the next `Present` it is attached to a private read FBO and blitted into the helper's texture.
- **Cost.** Only the armed frame does GL work in `SetRenderTarget`; elsewhere it is a pointer compare. Everything is skipped on a
  thread without a current GL context. A gameplay frame makes about 400 `SetRenderTarget(0)` and 130 `GetSurfaceLevel` calls.
- **`rt-trace`.** Twice per session (also without the helper) one frame of `SetRenderTarget(0)` calls is logged: surface, label,
  size, what GL reports (framebuffer, texture/renderbuffer name, size, samples), next target. It starts on the first frame after a
  *busy* frame (>= 50 calls), covers up to 1500 events, and prints the first 150 runs plus a complete per-transition summary. It is
  the discovery tool for eye-target candidates.

### Choosing the source

`target=` in `bmvr_target.txt` or `BMVR_TARGET`:

| Spec | Meaning |
|---|---|
| `name:<substring>` (default `name:_rt_fullframefb`) | render target whose engine label contains it, case-insensitive |
| `size:WxH` | any render target whose GL attachment has exactly that size |
| `window` | the default framebuffer (stage 2a) |

The configuration that came out of the first real run is `target=InternalRT0` (the 3840x2160 back buffer), `flip=0`.
With `flip=alt` successive snapshots alternate orientation and the helper writes it into the file name
`bmvr_snapshot_<n>_<target>_flip<0|1>.ppm`; `flip0` turned out to be upright.

### Real-game findings

- `libtogl.so` exports `GetSurfaceLevel`; `InternalRT0` was delivered through the pipe in gameplay.
- During loading screens the engine draws straight to the back buffer without `SetRenderTarget(0)`, so nothing is "left". Stage 3b
  handles this ([gpu-sync.md](gpu-sync.md)).
- The statistics label `registry=205736` was the cumulative put count, not a leak; renamed `registry puts=`.

### Trying it (snapshot sink)

Start `"<game>/bmvr_xfer_helper"` in a terminal before the game, load a map, stay about 15 s (the first `rt-trace` is at tick
~360). Send the helper output, the `xfer:` and `rt-trace:` lines of `bmvr_linux_log.txt` and which of `flip0` / `flip1` is upright.
Helper options for this mode: `--size WxH`, `--gpu N`, `--snapshots N`.

### Stand-in coverage

The default spec captures the right object via the private read FBO; lazy binding without a draw captures nothing; MSAA refused;
`size:`, `window`, garbage spec; no `GetSurfaceLevel`; no GL context; helper vanishing; GL state fully restored after every
`Present`; no blit with scissor enabled. Still stand-in only: float / A2R10G10B10 targets blitting into the RGBA8 image.
