# Linux support

Two separate things live here. Keep them apart.

| | Black Mesa **through Proton** (Windows build) | Black Mesa **native Linux build** |
|---|---|---|
| Status | Reported working (see `docs/open-work-2026-10.md`: PSVR2, SteamVR on Linux, OpenXR helper) | **Not working. Milestone 1 done (observe-only shim); stage 2a (GPU hand-off) and stage 2b (engine render target through the hand-off) run in the real game; stage 3a (GPU semaphores instead of glFinish, opt-in) run in the real game and works; stage 3b (loading-frame fallback, helper-death check before the GPU wait): the helper-death guard is verified in the real game by the kill loop (10/10 `kill -9` cycles, `sync=sem`, no hang), the loading-frame fallback is still stand-in only; stage 3c (one helper per game folder) added, tested without Vulkan; two-eye hand-off (stage 3d, protocol v4) is implemented, passes the stand-in tests (T14-T19) and is verified in the real game: pairs delivered, kill loop 30/30 with two eyes, 32-bit stand-in suite passes; eye 1 content is not proven yet (`_rt_hud` came out all black, probably an empty HUD layer).** |
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

## Stage 2b: an engine render target through the hand-off (run in the real game; results below)

Question it answers: can the shim find the GL object behind a Direct3D render target (without
ToGL's private structs) and deliver *that* through the stage 2a pipe, instead of the window? The
eye targets of the later stages are such render targets.

Same transport as 2a (protocol bumped to **v2**: the frame message now carries the target label,
the GL object name and a flip flag; build shim and helper together). What changed in the shim:

- **Registry.** `CreateTexture` (render-target usage), `CreateRenderTarget` and
  `IDirect3DTexture9::GetSurfaceLevel` (level 0) are interposed and record label + size per D3D
  object. `GetSurfaceLevel` **is exported and called** (confirmed in the real game, ~130 calls per
  frame); ToGL hands out a fresh surface object on nearly every call, so the registry is a fixed
  4096-slot hash that overwrites by pointer (memory is bounded; the statistics line counts *puts*).
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
- **`rt-trace`.** Twice per session (also **without** the helper; stage 3a starts it on the first *busy* frame, see below) one frame of
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

### Real-game result (stage 2b)

From the first real run (RTX 3080, native build under the Steam Linux Runtime; the config that came
back selects `target=InternalRT0`, `flip=0`):
- `libtogl.so` exports `GetSurfaceLevel`: **confirmed**.
- A gameplay frame makes about **400** `SetRenderTarget(0)` and about **130** `GetSurfaceLevel`
  calls (60 fps, vsync). That is the scale any per-frame hook has to live with.
- `InternalRT0` (the back buffer, 3840x2160) was delivered through the pipe in gameplay.
  During loading screens no snapshot is possible: the engine draws straight to the back buffer
  without `SetRenderTarget(0)`, so no target is ever *left* in the armed frame.
- The `rt-trace` was wasted: it fired at fixed ticks, which were loading-screen frames, so it
  logged nothing. Fixed in stage 3a (trace on a busy frame, with a per-transition summary).
- The statistics line said `registry=205736 (... surfaces)`. That was the **cumulative put count**,
  not a leak; renamed to `registry puts=`.
- **A real bug the stand-in tests then exposed (fixed in stage 3a):** the registry was one 4096-slot
  table shared by long-lived entries (render-target textures, the back buffer, registered once) and the
  short-lived `GetSurfaceLevel` surfaces (~130 fresh objects per frame). Once the churn saturated the
  table every put evicted a random entry, so a long-lived target lost its label within seconds and was
  never found again. In the first real run, snapshots worked shortly after loading and the log shows
  no later delivery, which fits this (the log's per-message cap hides it, so it is a strong suspicion,
  not proof). It showed up in the tests as a ~1-in-25 flake, then deterministically once the fake
  allocator produced as many distinct addresses as the real one.

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

**Not verified when this was written (the first real run settled some, see "Real-game result": `GetSurfaceLevel` is exported, `InternalRT0` captures in gameplay, the 32-bit build links on the dev machine):** that ToGL really
binds lazily the way the stand-in assumes (if it binds eagerly, the guard will count mismatches
and the `rt-trace` block will show it); that `_rt_FullFrameFB` is a texture (not a renderbuffer or
MSAA) on this build; the orientation; float / A2R10G10B10 targets blitting into the RGBA8
shared image as expected; that the shim runs on the thread that owns the GL context; and the
32-bit build itself (this machine has no 32-bit libc: the shim was compiled and tested as 64-bit
from identical source and `-fsyntax-only` checked, the 32-bit link happens in
`scripts/build-linux.sh` on yours).

## Stage 3a: GPU semaphores instead of glFinish (opt-in; run in the real game, see "Real-game result (stage 3a)")

Until now every snapshot cost a full `glFinish` (the shim waits for the *whole* GPU queue of the
game) plus a socket round trip. That is fine for pictures, not for a per-frame hand-off. Stage 3a
orders the two APIs on the GPU instead:

    GL (shim):   wait(vk_done) [not before the first frame] -> blit -> signal(gl_ready) -> glFlush
    helper:      wait(gl_ready) -> copy the image -> signal(vk_done)

- **Protocol v3** (`bmvr_xfer.h`). If the helper offers it (`hello.caps` has
  `BMVR_XFER_CAP_SEMAPHORES`) the hello carries three fds: the image memory and two
  `VK_KHR_external_semaphore_fd` opaque-fd binary semaphores (fresh pair per game connection). The
  shim imports them with `GL_EXT_semaphore_fd`. Frames carry `BMVR_XFER_FLAG_SEMAPHORE`; the ack
  then means "queue submission done" (so the helper's signal is *pending*, which is what makes the
  game's next GPU wait legal), not "readback finished".
- **Opt-in**: `sync=sem` in `<game folder>/bmvr_target.txt` or `BMVR_SYNC=sem`. Default stays
  `sync=finish`, the verified path. The shim falls back to glFinish + ack, and says why in the log,
  when the helper offers no semaphores, GL lacks the entry points, or the driver rejects the import.
- **Measuring**: every delivered snapshot logs `GL side N us` (time in the game's render thread) and
  `helper round trip N us`, with running averages. Run once per mode and compare.
- **Helper self-test**: `bmvr_xfer_helper --selftest-sync` (also run by `scripts/build-linux.sh`)
  checks that the GPU can export *and import* opaque-fd semaphores and runs 50 frame cycles with the
  helper playing the GL side through the re-imported fds. It needs no game.
- **Risk, which is why it is opt-in**: a GPU wait on a semaphore whose signal never arrives does not
  time out. If the helper is killed after it acked but before its GPU work ran, or the game dies
  between signalling and sending, the other side can wait forever. The helper detects its half
  (5 s fence timeout, then exits with an explanation); the game's GL side cannot. Restart the game.

Also in this stage: **the registry is now two tables** (pinned: render-target textures and
`CreateRenderTarget` surfaces, displaced only by other pinned entries, oldest first; transient: the
`GetSurfaceLevel` surfaces, an overwrite-at-will cache). `tests/linux` has a regression test (long run,
tens of thousands of distinct surface addresses): with the old single table it fails 10 checks,
including `InternalRT0` stopping after 2 snapshots; with the pinned table it passes.

From the real-game log: `rt-trace` now starts on the first frame after a *busy*
frame (>= 50 `SetRenderTarget(0)` calls), covers up to 1500 events, prints the ordered sequence (first 150
runs) and a complete per-transition summary with counts; the statistics label was corrected.

### What is verified and what is not (stage 3a)

Verified (`tests/linux/run-tests.sh`: the real 32-bit shim, a fake `libtogl` with lazy FBO binding, a
mock GL that records every call, a mock helper with real unix-socket fd passing; plus the real helper
on Mesa's software Vulkan driver with the Khronos validation layer):
- default path unchanged: one glFinish per snapshot, no semaphore calls, GL bindings preserved;
- `sync=sem`: both semaphores imported as OPAQUE_FD, per frame exactly `[wait] blit signal flush`, no
  glFinish, no wait before the first frame, each wait only after the helper acked the previous frame,
  barrier = the shared texture in layout GENERAL;
- every fallback (no semaphores offered, entry points missing, import rejected), protocol v2 vs v3
  mismatch, helper dying mid-session (semaphores deleted, game continues);
- renderbuffer and texture targets and the window source, in both sync modes;
- trace triggers on the first gameplay frame after loading frames;
- real helper: v3 hello, snapshot files named by target (unsafe characters sanitised), a semaphore frame
  it never offered is rejected without crashing it, selftest passes with the validation layer silent.

**Not verified (needs the real GPU):** that the driver really exports/imports the semaphores by fd (the
software driver cannot, so the fd path of the self-test was *not* exercised there), that
`GL_EXT_semaphore_fd` waits/signals order correctly against Vulkan on NVIDIA, image correctness and
tearing under `sync=sem`, and the actual cost difference.

### Real-game result (stage 3a)

Two runs, RTX 3080, driver 620.70.06, same map path, helper started first (`sync=finish` and `sync=sem`):
- The driver exports **and** imports the opaque-fd semaphores (`opaque-fd semaphore: extension=1 exportable=1
  importable=1`), the shim imported both (`GL semaphores 1/2 imported`), and `sync=sem` delivered every armed
  gameplay frame (5 PPM snapshots + 1 more in the log) with `synced by GPU semaphores`.
- Cost per delivered snapshot, time in the game's render thread: `sync=finish` 2.0-4.4 ms (average 3.05 ms over 10),
  `sync=sem` 79-112 us. The `sync=finish` helper round trip (about 1.07 s) is dominated by the helper writing a PPM
  file (the one unsaved snapshot took 24 us), so it is not comparable to the `sync=sem` round trip (45-89 us, where the
  ack only means "submitted"); compare the render-thread number.
- The five `sync=sem` snapshots were inspected: upright with `flip=0` (the default is right), consistent consecutive
  gameplay frames (tram ride, then a guard on a catwalk, plus the main menu), no Steam/MangoHud overlay, no tearing
  or stale-frame artefacts visible at half size. Single-scanline tearing was not checked pixel by pixel.
- The "no matching render target was left in the armed frame" lines in both logs were **not** a matching problem: they
  fall on loading frames (the stats window around them shows `SetRenderTarget=0`), and the line printed counters that
  were never reset, so the same `leaves seen=127 ... size-mismatch=25` repeated from an older frame. Fixed in 3b.
- Not verified at the time of 3a: a helper killed at the worst moment on the real GPU. Verified later, see "Stage 3b real-game kill loop" below.

## Stage 3b: loading frames and a hung-GPU guard (stand-in tests only)

Two fixes from the stage 3a real-game logs, plus a diagnostic fix:

- **Loading / menu frames.** The engine draws straight into the back buffer there (no `SetRenderTarget(0)` call), so
  nothing is "left" and no snapshot used to be delivered. If the armed frame made 0 `SetRenderTarget(0)` calls, the shim
  now re-checks the last delivered object (`glIsTexture` / `glIsRenderbuffer` if available, then its size, single-sample)
  and delivers that. The log says `loading/menu frame ... delivering the last known object of 'X'`. If the object is gone
  or resized it is forgotten and the shim waits for a gameplay frame. A loading frame before any gameplay frame is silent.
  Assumption to confirm in the real game: the back buffer's GL object (`InternalRT0`, FBO 9, texture 1 in the 3a log) holds
  the loading-screen picture at the end of those frames.
- **Helper death before the GPU wait.** A GL wait on a semaphore has no timeout, so a helper that acked and then died would
  hang the game at the next frame's `glWaitSemaphoreEXT`. Before each snapshot the shim now peeks the helper socket
  (`MSG_PEEK | MSG_DONTWAIT`); a closed socket tears everything down and goes back to searching, without issuing the wait.
  This also removes the up-to-2 s receive timeout in `sync=finish` when the helper is gone. Not covered: a helper that is
  alive but whose GPU work never finishes (its own 5 s fence timeout handles that side).
- **Diagnostics.** The armed-frame counters are reset when a frame is armed, and the "no matching render target" line now
  prints the frame's `SetRenderTarget(0)` count.

Tests (`tests/linux`, 32-bit shim + stand-ins, all pass; the new cases fail on the stage 3a shim):
`T12` helper exits right after acking a frame: socket check fires, no `glWaitSemaphoreEXT` / blit / signal afterwards,
semaphores deleted, game clean; `T13` loading screens before and after gameplay: fallback delivers snapshots during the
second loading screen, stays quiet before gameplay and when no fallback applies. `T10`'s stats-label check now reads the
shim binary (the stats line needs 5 s of wall time, the fake game takes well under 1 s). Needs `gcc -m32` (on Ubuntu:
`gcc-multilib g++-multilib`).

## Stage 3b real-game kill loop (verified 2026-11-19)

`tests/linux/test_kill_loop.sh` (bash script, runs from fish/zsh/bash because of its shebang, do **not** paste or
`source` it in fish) starts a helper, waits for `xfer: helper connected` in `bmvr_linux_log.txt`, waits 0-5 s at random,
`kill -9`s the helper and waits for the shim to log `lost the helper` / `helper's socket is closed` / `back to searching`.
The next cycle's connect line proves the shim reconnected. It stops at the first failed cycle and prints the last log lines.

- Real run, Black Mesa native, RTX 3080, `bmvr_target.txt` = `target=InternalRT0`, `flip=0`, `sync=sem`: **10/10 cycles
  passed** (the run was interrupted by hand after cycle 10; the script's default is 30). No GPU hang, no crash, the shim noticed
  every dead helper and reconnected to the next one. This closes the "helper killed at the worst moment" item of stage 3a.
  Ten random kill moments is a sample, not a proof; run the default 30 once in a map for more confidence.
- The first attempt failed with "shim never connected to the new helper": an earlier helper was still running and the game
  was still connected to it. The helper used to `unlink()` and re-`bind()` the socket path unconditionally, so a second helper
  took the path over silently and listened for ever. The test now refuses to start while a helper runs (`pgrep -f`; `pgrep -x`
  cannot be used, the process name is 16 characters and the kernel keeps 15), and 3c fixes the cause.
- The shim retries the connection every `XFER_RETRY` = 240 presents (about 4 s at 60 fps, much longer on a slow loading
  screen), so the script waits up to 45 s for a reconnect.

## Stage 3c: one helper per game folder

`bmvr_xfer_helper` takes an exclusive, non-blocking `flock()` on `<game folder>/bmvr_xfer.lock` **before** it touches the GPU
and exits with an error naming the holder's pid when another helper already serves that folder. The kernel drops the lock
when the process dies, including `kill -9`, so there is no stale-lock cleanup (the kill loop depends on this). Code:
`linux/bmvr_single_instance.h` (header-only so the test uses the very same function). `--selftest-sync` does not lock.
Helpers started from different game folders do not interfere. An old helper binary (before 3c) does not take the lock, so
after upgrading make sure no old helper is running.

Tests: `tests/linux/lock_test.c`, run by `tests/linux/run-tests.sh` before the stand-in suite (no Vulkan, no GPU): second
holder refused and the holder pid reported, lock free again immediately after `kill -9`, unwritable folder gives a clear
error. **Not run against the real helper yet**: the code was added to `linux/bmvr_xfer_helper.c` but that file needs the
Vulkan headers, which were not available where it was written. Build it with `scripts/build-linux.sh` and check:
start one helper, start a second one in another terminal; the second must print `ERROR: another bmvr_xfer_helper already
serves this game folder ... (pid N)` and exit 1, and the first must keep running.

## Stage 3d: two-eye hand-off (protocol v4; implemented, stand-in tested)

Design and reasoning: `docs/LINUX_STEREO_HANDOFF.md`. Short version: a **pair** of eye images is the unit of publication
(one frame message, one wait before the first blit, one signal after the second, one ack), so the two eyes of a pair
always come from the same game frame.

- `bmvr_xfer.h` **v4**: hello 40 -> 48 bytes (`eye_count`, `reserved`), one memory fd per image then the two semaphore fds;
  frame 48 -> 92 bytes (`eye_count`, `eye1_src_w/h`, `eye1_flags`, `eye1_gl_name`, `eye1_name`). Shim and helper must be
  rebuilt together (a v3 peer is rejected with both versions in the log).
- Helper: `--eyes 1|2` (default 1). Two images + two readback buffers, ONE semaphore pair; snapshots of a pair are
  `bmvr_snapshot_<n>_eye0_<what>_flip<f>.ppm` / `..._eye1_...`; single-eye names are unchanged.
- Shim: `target2=name:<label>` or `target2=size:WxH`, `flip2=0|1|alt` (default: same as `flip`) in `bmvr_target.txt`, or
  env `BMVR_TARGET2` / `BMVR_FLIP2`. Eye 1 is used only when `target2` is set AND the helper offers two images
  (`--eyes 2`); otherwise the shim says why in the log and delivers eye 0 only, exactly as before. `target2` is ignored when
  eye 0 is `window`. A pair is delivered only when BOTH eyes were captured in the same armed frame; otherwise nothing is
  sent (log line `no matching render target for eye 1 ...`). The loading/menu fallback of 3b works per eye.
- The engine still renders ONE view, so on the real game a pair is two *existing* render targets (or the same one twice).
  That proves the transport, not stereo. A real left/right picture needs the game-hook port (plan item 4).

**Tested** (stand-ins, `tests/linux`): T14 pair ordering `WAIT BLIT BLIT SIGNAL FLUSH frame ack`, one signal per pair, both
textures in the barrier lists, distinct blits; T14b `BLIT BLIT FINISH`; T14c eye 1 by size with its own flip; T15 no half pair
when eye 1 disappears; T16 negotiation (helper offers 2/no target2, target2/helper offers 1, window); T17 helper death
after a pair; T18 v3 peer and `eye_count` 3 rejected; T19 loading frames with two eyes. T1-T13 (single eye) still pass
unchanged. These ran as a host-native (64-bit) build (`BMVR_TEST_NATIVE=1`), because the machine they were written on has no
32-bit toolchain; **run the normal `tests/linux/run-tests.sh` (32-bit) once on your machine**: it is the authoritative run.
**Real game (2026-11-20, RTX 3080, `sync=sem`, `target=InternalRT0`, same object for both eyes, helper `--eyes 2`):**
- Manual run: 5 pairs (ticks 240, 360, 480, 600, 720), `eye0` and `eye1` snapshot files written for each, 99.7-100% non-black, the
  non-black percentage identical for both eyes of every pair (expected: same source, same flip). The log reports both GL
  textures; no hang, no error.
- `tests/linux/test_kill_loop.sh -n 10 --eyes 2`: **10/10 cycles passed**, each one requiring the shim to have imported both eyes
  and to notice the dead helper and reconnect. The per-cycle helper outputs show helpers killed at random points, including
  between writing the `eye0` and the `eye1` file of one pair (the helper process dies mid-save: expected, the game is unaffected).
- Same day, later runs: the default **30 kill cycles with `--eyes 2`: 30/30 passed**; the plain 32-bit `tests/linux/run-tests.sh` on
  the author's machine: lock test, T1-T19 and the real-helper section all passed (selftest PASS on the RTX 3080; the real helper
  wrote `eye0`/`eye1` files for pairs it was sent).
- **`target2=name:_rt_hud`** (a genuinely different second target): pairs delivered every time (5 of 5, the same tick for both
  eyes, no error). Eye 0 holds a real picture (a 3D scene at tick 240, a "Loading resources" screen by tick 720). Eye 1 is **exactly
  (0,0,0) in every pixel of all 5 snapshots** (helper: "all black"). Most likely the HUD layer was simply empty: the snapshots cover
  the first ~12 s after the helper connects, and the game showed a menu/background scene and a loading screen, where no HUD is
  drawn, and an RGB readback of a cleared transparent layer is black. **Not proven**: it could also be that the blit reads a
  stale or wrong object. The transport itself is not in question (pairs, semaphores, reconnects all behave), but no non-zero
  pixel has reached the helper through eye 1 yet.
- Next, to settle it: (1) `target2=name:_rt_gblightdiff` (a full-size buffer filled on every 3D frame in the rt-trace); (2) with
  `_rt_hud`, start the helper while a map is running with the HUD visible (the game reconnects within a few seconds; `--snapshots 20`
  keeps more frames). If (1) shows a picture and (2) a HUD, eye 1 is proven; if (1) is also black, the blit of a second object
  is wrong and the shim log (`xfer:` lines) is what to send.

Candidate second targets from the real rt-trace (all 3840x2160, same size as `InternalRT0`): `_rt_hud` (the HUD layer, mostly empty),
`_rt_gblightdiff` (light buffer), `_rt_gbdepthrt` (depth). `_rt_hud` was tried first (see above): all black.

Trying it in the real game (the same steps as the kill loop, plus):
1. Rebuild with `scripts/build-linux.sh` (shim and helper together), stop any old helper.
2. `bmvr_target.txt`: `target=InternalRT0`, `flip=0`, `sync=sem`, and for the first test the SAME object for both eyes:
   `target2=InternalRT0` (proves two images + semaphores with known-good content). Later point `target2` at a different
   target from the rt-trace output.
3. `bmvr_xfer_helper --eyes 2`, start the game: the log shows `imported ... GL textures N (eye 0) and M (eye 1)`; the helper writes
   `bmvr_snapshot_<n>_eye0_...` and `..._eye1_...`.
4. `tests/linux/test_kill_loop.sh -n 10 --eyes 2`: a cycle fails loudly if the shim used one eye only.

## Stage 4a: native OpenXR sink (X0 implemented; design in `docs/LINUX_OPENXR_CONSUMER.md`)

The helper gets an OpenXR sink (same process: `XR_KHR_vulkan_enable2` makes the runtime own the Vulkan device, so the exportable
images must live on it). Milestone X0 exists: `bmvr_xfer_helper --xr-probe` opens the runtime (loader, instance, system,
runtime-chosen Vulkan device, session, swapchain), submits no frames and ends with PASS / FAILED / UNAVAILABLE (exit 0 / 1 / 2).
`tests/linux/test_xr_probe.sh [--software]` runs it against a headless Monado. Observed so far: Monado 21.0.0 in a sandbox without
a GPU or headset (14/14 checks; see the "X0 results" table, including what that does **not** show). **SteamVR: X0 PASS observed once** (user log, SteamVR/OpenXR 2.19.4, NVIDIA RTX 3080 on Vulkan 1.4, 3036x3036 per eye, R8G8B8A8_SRGB offered, opaque-fd image and semaphore export OK, session reaches READY; no frames submitted). It first failed `UNAVAILABLE` (-51) until SteamVR was made the active OpenXR runtime, see below. Not checked: the game feeding the sink, frames in the headset. `--sink xr` (X2) refuses to start: "not implemented yet". Nothing here changes the snapshot sink, T1-T19 or the kill loop.

Try it (any OpenXR runtime running; no game needed): `"$GAME/bmvr_xfer_helper" --xr-probe` and send the output.

**SteamVR must be the active OpenXR runtime.** A running SteamVR is not enough. Without an active runtime the probe prints
`RuntimeManifestFile::FindManifestFiles - failed to determine active runtime file path`, `xrEnumerateInstanceExtensionProperties
failed (-51)` and ends `UNAVAILABLE`. Fix (observed to work; SteamVR's Settings -> Developer button "Set SteamVR as OpenXR
runtime" was missing on that install):

    mkdir -p ~/.config/openxr/1
    ln -sf ~/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json ~/.config/openxr/1/active_runtime.json

Other Steam library: adjust the path (check `steamxr_linux64.json` exists first). Flatpak Steam: paths are under
`~/.var/app/com.valvesoftware.Steam/` and the sandbox needs access to the OpenXR config. Per launch instead of system-wide:
`XR_RUNTIME_JSON=<SteamVR dir>/steamxr_linux64.json`. Not tested: Flatpak, per-launch override. Needs a headset (or null
driver) connected to SteamVR, otherwise a "no system" error can follow once the runtime loads.

## Plan (not implemented)

1. ~~Read the milestone-1 log; confirm eye-target candidates and driver extensions.~~ Done.
2. ~~Stage 2a: test the transport.~~ Done, verified. ~~Stage 2b: capture an engine render target.~~ Done, verified in the real game (`InternalRT0`).
3. Stage 3a: ~~replace `glFinish` + ack with `GL_EXT_semaphore_fd` / `VK_KHR_external_semaphore_fd`~~ done, verified in the real game (opt-in). Stage 3b: loading-frame fallback + helper-death guard (helper-death verified by the kill loop; loading fallback awaiting a real run). Stage 3c: one helper per game folder (flock).
   Stage 4a (OpenXR sink, `docs/LINUX_OPENXR_CONSUMER.md`): X0 probe done on Monado; X1-X4 not started.
   Then (design in `docs/LINUX_STEREO_HANDOFF.md`): render a *separate* stereo pair into two eye targets (the engine renders one view today) and
   hand both to a 64-bit native OpenXR helper. Which targets and sizes to use is decided by the
   `rt-trace` output.
4. Find and port the ~100 game hooks (Itanium ABI: cdecl with `this` first, destructor
   slot shift in vtables, stripped code -> anchor/RTTI scanning).
5. Port hands / weapon wheel / HUD drawing off D3D9-on-DXVK.
6. Introduce a platform layer so shared code stops depending on `windows.h`, MinHook and SEH.
