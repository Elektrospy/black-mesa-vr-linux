# Linux support

Two separate things live here. Keep them apart.

| | Black Mesa **through Proton** (Windows build) | Black Mesa **native Linux build** |
|---|---|---|
| Status | Reported working (see `docs/open-work-2026-10.md`: PSVR2, SteamVR on Linux, OpenXR helper) | **Not working. Milestone 1 done (observe-only shim); stage 2a (GPU hand-off) verified in the real game; stage 2b (engine render target through the hand-off) implemented and tested against stand-ins, NOT yet run in the real game; stage 3 (semaphore sync, real eye targets) next.** |
| Mod | The normal `d3d9.dll` + `OpenXRHelper64.exe` | `linux/` (this port, not a VR mod yet) |
| Install | `scripts/bmvr-linux.sh` | `scripts/build-linux.sh` |

Nothing in the Windows build changed. The only edit to existing files is a guarded
`add_subdirectory(linux)` in the root `CMakeLists.txt` that is skipped whenever
`WIN32` is set (that includes the clang-cl/xwin cross build, which sets
`CMAKE_SYSTEM_NAME=Windows`).

## Proton: `scripts/bmvr-linux.sh`

Port of the README install steps for the release zip (`drop-into-Black-Mesa-folder`),
plus the cleanups `scripts/install.ps1` does. Finds the game in the Steam libraries
(`libraryfolders.vdf`, Flatpak, Snap), refuses to run while `bms.exe` or the helper is
running, installs `d3d9.dll` into all three required places (AGENTS.md rule 19), backs up
stock DXVK / `openvr_api.dll` once, keeps an existing `VR/config.txt`, writes the VR
`dxvk.conf`, disables `bin/sourcevr.dll`, clears stale `bmvr_in_*.flag` files and false
`bmvr_skip.txt` bans. `uninstall` reverses it.

Launch options it prints (the `WINEDLLOVERRIDES` part is **unverified**; drop it if the
DLL loads without it):

    WINEDLLOVERRIDES="d3d9=n,b" %command% -heapsize 524288 -processheap -high -novid -windowed -oldgameui -enabledxvk

### Separate copy, Steam library entry, Blue Shift

    scripts/bmvr-linux.sh install --separate                          # copy + install the mod into the copy only
    scripts/bmvr-linux.sh install --separate --steam-shortcut         # ... plus a Steam library entry (Steam closed)
    scripts/bmvr-linux.sh install --separate --steam-shortcut --campaign bms --campaign bshift

`--separate` makes a copy-on-write copy (plain copy when the filesystem has no reflinks; never
hardlinks) named "Black Mesa VR" next to the original and installs only there, so the original
game and its normal launch stay untouched and both can be started independently. The copy has
its own saves and settings. `--steam-shortcut` adds a non-Steam shortcut per `--campaign`
(`bms`, `bshift` = Blue Shift `-game bshift`, `asheep` = Azure Sheep `-game asheep`) to
`userdata/<id>/config/shortcuts.vdf`, with a timestamped backup, and refuses to touch a file it
cannot rewrite byte for byte. Proton must still be selected for the shortcut in Steam
(Properties -> Compatibility). Blue Shift: the mod detects `-game bshift` itself and always
reads `bms/cfg/bmvr.cfg`, so the same install covers it. The shortcut writer was tested only
against hand-built files, not a live Steam; `steam_appid.txt` for the copy is unverified.

Needs the Windows build of the game: force a Proton tool in Steam (the folder must contain
`bms.exe`). The mod patches fixed offsets in the Windows `client.dll` / `server.dll`.

## Native Linux: why it is a different architecture

AGENTS.md says to follow L4D2VR's architecture and to document divergences once the
failure is verified. These are the verified facts (from `tools/linux/bmvr-probe*.sh`
output on a CachyOS x86_64 machine; native build files from Steam):

- The native build is **32-bit x86** (`bms_linux`, `bin/launcher.so`, all game libs).
- It renders through **ToGL** (`bin/libtogl.so`, Direct3D 9 on OpenGL). `launcher.so` and
  `shaderapidx9.so` reference `libtogl.so`; **no binary or script mentions DXVK**.
  `bin/libdxvk_d3d9.so` ships in the folder but nothing found loads it. So the DXVK-based
  L4D2VR path (`IDirect3DVR9`, Vulkan eye textures) is not selectable on this build.
- `libtogl.so` **exports its D3D9 classes by name** (`IDirect3DDevice9::Present`,
  `SetRenderTarget`, `StretchRect`, `CreateTexture`, `CreateRenderTarget`, `Reset`,
  `IDirect3D9::CreateDevice`, `Direct3DCreate9`, ...) and `shaderapidx9.so` imports 123
  D3D symbols from it. These can be interposed with `LD_PRELOAD`.
- Game code is **stripped**: `client.so` exports 383 dynamic symbols, the others about
  six. Hook points must be found the way the Windows ones were (anchor strings, RTTI).
  `client.so` still contains anchors such as `CViewRender::RenderView`,
  `CViewRender::ViewDrawScene`, `CViewRender::DrawViewModel`, `CViewRender::SetUpViews`,
  `CClientShadowMgr::UpdateFlashlightState` and RTTI names (`11CViewRender`,
  `15C_BaseViewModel`).
- Interface names match the Windows build: `VClient018`, `VEngineClient015`,
  `VMaterialSystem081`, `VEngineRenderView014`.
- The game's bundled `bin/libopenvr_api.so` is the old (pre-1.0, `VR_Init`) ABI.
- SteamVR on the test machine appeared to have no `bin/linux32`. A 32-bit game therefore
  needs a **64-bit helper** for OpenXR/OpenVR, as on Windows.

## Milestone 1: `linux/bmvr_preload.c`

A 32-bit `LD_PRELOAD` shim that **only observes**. It forwards every call unchanged. It
logs `bmvr_linux_log.txt` in the game folder with:

1. proof that the ToGL interposition works in the real game,
2. every render target / large texture the engine creates (size, format, debug label),
3. a once-per-session GL report: vendor, renderer, version and whether
   `GL_EXT_memory_object_fd` / `GL_EXT_semaphore_fd` exist (needed for the planned
   Vulkan -> GL texture hand-off to the 64-bit helper),
4. a frame-rate / call-count line every 5 s.

Build and install (native build only, **not** Proton):

    scripts/build-linux.sh --auto                                  # finds the native game in your Steam libraries
    scripts/build-linux.sh "/path/to/steamapps/common/Black Mesa"  # or pass the folder (or set BMVR_GAME_DIR)

Then set the Steam launch option it prints and send `bmvr_linux_log.txt`:

    "<game folder>/bmvr-run.sh" %command%

Why two launcher files: Steam expands `"<wrapper>" %command%` to
`<wrapper> steam-launch-wrapper -- reaper ... -- scout-on-soldier-entry-point-v2 -- <game>/bms.sh ...`,
so the wrapper runs on the HOST, outside the Steam Linux Runtime container (pressure-vessel), which
rebuilds `LD_PRELOAD` and drops what was set outside (observed: the game process had only Steam's
overlay entries). `bmvr-run.sh` (host) therefore swaps the `bms.sh` argument for `bmvr-bms.sh`, a new
file in the game folder that runs inside the container, sets `LD_PRELOAD` and `LD_LIBRARY_PATH`, and
then runs the original `bms.sh`, which is never modified. Nothing needs patching or undoing;
`scripts/build-linux.sh --uninstall` deletes the files this port added. It uses the bare name
`bmvr_preload32.so`, not a path: `LD_PRELOAD` splits on spaces and the game folder is "Black Mesa".
The shim is installed to `<game>/bin/`. Markers: `bmvr_run_marker.txt` (host wrapper ran, and whether
the swap happened) and `bmvr_inner_marker.txt` (container side ran).
If no log appears: `tools/linux/bmvr-diagnose-linux.sh` (run it while the game is running).
The shim also avoids glibc symbols newer than 2.1.3 so an older Steam runtime can load it.
VS Code: `.vscode/tasks.json` has build / build+install (auto) / build+install (choose folder).

### What is verified and what is not

Verified (on a Linux dev machine, with stand-ins):
- Root CMake: patch applies cleanly; native configure + build produce a 32-bit ELF that
  exports exactly the seven ToGL names; a Windows-style configure still reaches the
  original `Black Mesa is Win32` check; a 64-bit native configure fails with a clear message.
- Interposition mechanics: against a stand-in `libtogl.so` with the same C++ signatures,
  called through the PLT, every call is forwarded with correct arguments and return values.
- GL report and log rate-limiting against a mock SDL/GL.

**Verified in the real game** (NVIDIA RTX 3080, driver 620.70.06, CachyOS, Steam Linux Runtime
soldier): the launcher chain (host wrapper -> `bmvr-bms.sh` -> original `bms.sh`) puts the shim
into `bms_linux`; `CreateDevice` / `Present` / `CreateRenderTarget` are interposed and forwarded;
the render-target list is logged (3840x2160 `_rt_fullframefb`, `_rt_fullscreen`, `_rt_hud`,
`_rt_gbdepth`, CSM shadow targets, ...); the GL report works and shows `GL_EXT_memory_object_fd`
and `GL_EXT_semaphore_fd` are available. Gotcha found on the way: the engine loads `libtogl.so` and
SDL with `RTLD_LOCAL`, so `dlsym(RTLD_NEXT/RTLD_DEFAULT)` cannot see them; the shim looks them up
through an explicit `RTLD_NOLOAD` handle (see `find_loaded_lib`).

## Stage 2a: GPU hand-off test (`linux/bmvr_xfer_helper.c` + `xfer_*` in the shim)

Question it answers: can a 64-bit process share GPU memory with the 32-bit game? (Required for
OpenXR: the runtime is 64-bit, the game is 32-bit.)

    64-bit helper                                32-bit game (shim)
    Vulkan image, exportable memory   --fd-->    glImportMemoryFdEXT -> GL texture
    (VK_KHR_external_memory_fd)       (unix      glBlitFramebuffer(back buffer -> texture)
    reads the image back, writes      socket,    every ~120 presents, then glFinish
    bmvr_snapshot_N.ppm               SCM_RIGHTS) and waits for the helper's ack

Use it: start `"<game>/bmvr_xfer_helper"` in a terminal **before** the game (the installer puts
it there; it needs the Vulkan headers to build, see `scripts/build-linux.sh` output), start the
game from Steam, load a map for ~10 s. Result: `bmvr_snapshot_0.ppm` in the game folder (a
picture, scaled to 1920x1080 and upright) and `xfer:` lines in `bmvr_linux_log.txt`. Options:
`--size WxH`, `--gpu N`, `--snapshots N`. The helper prints its Vulkan device UUID; the shim logs
`GL device UUID`. They must match (relevant on hybrid-GPU laptops).

Design notes:
- The shim does nothing unless `<game>/bmvr_xfer.sock` exists; if the helper dies, the shim
  logs it, deletes its GL objects and goes back to looking. It never blocks longer than 2 s
  (socket timeouts), uses `MSG_NOSIGNAL`, and saves/restores the GL state it touches (FBO
  bindings, texture binding, scissor enable).
- Synchronisation here is a full `glFinish` plus an explicit ack message, so a frame is never read
  while it is being written. The real renderer must use `GL_EXT_semaphore_fd` /
  `VK_KHR_external_semaphore_fd` instead; that is the next step after this test works.
- The image is kept in `VK_IMAGE_LAYOUT_GENERAL` (an `UNDEFINED` old layout may discard
  contents).
- The test copies the default framebuffer, i.e. the final composed desktop frame, not an eye
  target. If ToGL renders to an offscreen target and only blits during `Present`, the snapshot
  could be black; the helper reports the share of non-black pixels and the shim logs the
  bound framebuffers, which tells us what to read instead.

**Result on real hardware (RTX 3080, driver 620.70.06):** it works. The helper's Vulkan device UUID
and the shim's GL device UUID were identical (`afc5d13f...`), the Vulkan image was exportable
(`exportable=1`, `dedicated-only=0`), the shim imported it, and `bmvr_snapshot_0.ppm` is the game
picture (main-menu background with the loading bar), upright, 3840x2160 scaled to 1920x1080, 99.7-100%
non-black pixels in all five snapshots. So a 64-bit process can read what the 32-bit game
renders through shared GPU memory. Two caveats seen in the snapshot: it contains the MangoHud and
Steam overlay drawing (it is the window's default framebuffer, i.e. what is on screen, and the game
itself renders into FBO 9: log line `read fb=9 draw fb=9`), and because the read happens before
ToGL's own `Present`, it may be the previous frame. Neither matters for the eye targets, which are
the engine's render targets, not the window. Performance is not measured yet (the test uses
`glFinish` and a round trip every ~2 s).

Before that result, the same code was checked with stand-ins on a Linux dev machine (no GPU there): the socket protocol and fd passing
against a mock helper; the exact sequence and arguments of the GL calls against a recording fake
GL (import, tiling, storage, flipped blit, state restore); helper death mid-session (no hang, no
crash, clean teardown and re-search); the 64-bit helper compiles and runs up to Vulkan device
enumeration. Still **not verified** after 2a: semaphore-based synchronisation and per-frame cost (stage 3).
Capturing an engine render target instead of the window is stage 2b below.

## Stage 2b: an engine render target through the hand-off (implemented; not run in the real game)

Question it answers: can the shim find the GL object behind a Direct3D render target (without
ToGL's private structs) and deliver *that* through the stage 2a pipe, instead of the window? The
eye targets of the later stages are such render targets.

Same transport as 2a (protocol bumped to **v2**: the frame message now carries the target label,
the GL object name and a flip flag; build shim and helper together). What changed in the shim:

- **Registry.** `CreateTexture` (render-target usage), `CreateRenderTarget` and
  `IDirect3DTexture9::GetSurfaceLevel` (level 0) are interposed and record label + size per D3D
  object. `GetSurfaceLevel` is a **new, unconfirmed export**: the earlier probe listing of
  `libtogl.so` was truncated, so it is not known that the symbol exists. If it does not, surfaces
  have no labels and the shim says so in the log (hint line) and falls back to selecting by size.
- **Resolving the GL object.** ToGL binds its framebuffer lazily, at draw time. So when the
  engine calls `SetRenderTarget(0, X)`, the GL *draw* framebuffer still holds the target being
  *left*. The shim asks GL what is attached to it (`glGetFramebufferAttachmentParameteriv`: type +
  name), measures it, and accepts it only if the size equals the registry size of the target being
  left. That guard also covers "no draw since the last `SetRenderTarget`" and reused pointers.
  Multisampled targets are refused (a scaling blit out of MSAA is not allowed).
- **Final content of the frame.** The last accepted leave of the target in the armed frame
  (the frame before each snapshot) is remembered as (type, GL name, size). At the next `Present`
  the shim attaches that object to a private read FBO and blits it into the helper's texture. So
  the picture is the target as it stands at the end of the frame, not a mid-frame state.
- **Cost.** Only the one armed frame per ~2 s does any GL work in `SetRenderTarget`; everywhere
  else it is a pointer compare. GL state touched (FBO bindings, texture / renderbuffer binding,
  scissor) is restored and the shim's own GL errors are drained. Everything is skipped on a
  thread without a current GL context.
- **`rt-trace`.** Twice per session (ticks ~360 and ~720, also **without** the helper) one frame of
  `SetRenderTarget(0)` calls is logged: surface, label, size of the target left, what GL reports
  for it (framebuffer, texture/renderbuffer name, size, samples) and the next target. This is the
  discovery tool for the real eye targets: it shows the order of targets in a frame and which ones
  resolve.

Choosing the source: env `BMVR_TARGET` or `<game folder>/bmvr_target.txt` (`target=...`, `flip=...`):

| Spec | Meaning |
|---|---|
| `name:<substring>` (default `name:_rt_fullframefb`) | render target whose engine label contains it, case-insensitive |
| `size:WxH` | any render target whose GL attachment has exactly that size (works without labels) |
| `window` | the default framebuffer, i.e. stage 2a |

Y orientation of a ToGL render target is not known, so by default (`flip=alt`) successive
snapshots alternate the flip and the helper writes it into the name:
`bmvr_snapshot_<n>_<target>_flip<0|1>.ppm`. One of each pair is upright; that settles the
default for stage 3.

Run it exactly like 2a (helper first, then the game; **load a map and stay ~15 s**, because
the first `rt-trace` is at tick ~360). Send: the helper terminal output, the `xfer:` and
`rt-trace:` lines of `bmvr_linux_log.txt`, and which of `flip0` / `flip1` is upright.

### What is verified and what is not (stage 2b)

Verified on a dev machine without a GPU, using stand-ins (64-bit build of the same shim source, fake
`libtogl` forwarding targets, a fake GL that models FBO state and **lazy FBO binding**, and a mock
helper that speaks protocol v2 over a real unix socket with fd passing):
- default spec captures the right object: the blit's source is the FullFrameFB texture, via the
  private read FBO, at full size, into the helper's texture; flip alternates; helper receives
  label / size / GL name / flags;
- lazy binding (target bound but not drawn): nothing is captured, mismatch counted;
- MSAA target: refused, no blit; `size:` spec; `window` spec; garbage spec keeps the default;
  `bmvr_target.txt` parsing; no `GetSurfaceLevel` (hint line, `size:` fallback works);
- no GL context on the calling thread: does nothing; helper vanishing mid-session: "lost the
  helper", back to searching, game loop continues; no helper at all: trace still logged;
- after every `Present` the fake GL's FBO / texture / renderbuffer bindings, scissor and error flag
  equal what the "engine" left, and no blit runs with scissor enabled;
- the shim needs no glibc symbol version above the 2.34 `dlopen`/`dlsym` pair already pinned by
  `.symver` on i386 (a `strtoul` that glibc >= 2.38 would have linked to `__isoc23_strtoul` was
  replaced for exactly this reason).

**Not verified (needs the real game):** that `libtogl.so` exports `GetSurfaceLevel`; that ToGL really
binds lazily the way the stand-in assumes (if it binds eagerly, the guard will count mismatches
and the `rt-trace` block will show it); that `_rt_FullFrameFB` is a texture (not a renderbuffer or
MSAA) on this build; the orientation; float / A2R10G10B10 targets blitting into the RGBA8
shared image as expected; that the shim runs on the thread that owns the GL context; and the
32-bit build itself (this machine has no 32-bit libc: the shim was compiled and tested as 64-bit
from identical source and `-fsyntax-only` checked, the 32-bit link happens in
`scripts/build-linux.sh` on yours).

## Plan (not implemented)

1. ~~Read the milestone-1 log; confirm eye-target candidates and driver extensions.~~ Done.
2. ~~Stage 2a: test the transport.~~ Done, verified. ~~Stage 2b: capture an engine render target.~~ Implemented, awaiting a real-game run.
3. Stage 3: replace `glFinish` + ack with `GL_EXT_semaphore_fd` / `VK_KHR_external_semaphore_fd`;
   render a *separate* stereo pair into two eye targets (the engine renders one view today) and
   hand both to a 64-bit native OpenXR helper. Which targets and sizes to use is decided by the
   `rt-trace` output.
4. Find and port the ~100 game hooks (Itanium ABI: cdecl with `this` first, destructor
   slot shift in vtables, stripped code -> anchor/RTTI scanning).
5. Port hands / weapon wheel / HUD drawing off D3D9-on-DXVK.
6. Introduce a platform layer so shared code stops depending on `windows.h`, MinHook and SEH.
