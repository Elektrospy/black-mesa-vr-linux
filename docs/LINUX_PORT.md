# Linux support

Two separate things live here. Keep them apart.

| | Black Mesa **through Proton** (Windows build) | Black Mesa **native Linux build** |
|---|---|---|
| Status | Reported working (see `docs/open-work-2026-10.md`: PSVR2, SteamVR on Linux, OpenXR helper) | **Not working. Milestone 1 only (observe-only shim).** |
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

Then set the Steam launch option it prints and send `bmvr_linux_log.txt`.
VS Code: `.vscode/tasks.json` has build / build+install (auto) / build+install (choose folder).

### What is verified and what is not

Verified (on a Linux dev machine, with stand-ins):
- Root CMake: patch applies cleanly; native configure + build produce a 32-bit ELF that
  exports exactly the seven ToGL names; a Windows-style configure still reaches the
  original `Black Mesa is Win32` check; a 64-bit native configure fails with a clear message.
- Interposition mechanics: against a stand-in `libtogl.so` with the same C++ signatures,
  called through the PLT, every call is forwarded with correct arguments and return values.
- GL report and log rate-limiting against a mock SDL/GL.

**Not verified**: anything inside the real game. The shim has not been run against the real
`libtogl.so` / `shaderapidx9.so`, no frame has been captured, and no VR output exists.

## Plan (not implemented)

1. Read the milestone-1 log; confirm eye-target candidates and driver extensions.
2. Capture the eye render targets (GL texture names via the bound framebuffer after
   `SetRenderTarget`/draw), hand them to a 64-bit native OpenXR helper through
   `VK_KHR_external_memory_fd` / `GL_EXT_memory_object_fd`.
3. Find and port the ~100 game hooks (Itanium ABI: cdecl with `this` first, destructor
   slot shift in vtables, stripped code -> anchor/RTTI scanning).
4. Port hands / weapon wheel / HUD drawing off D3D9-on-DXVK.
5. Introduce a platform layer so shared code stops depending on `windows.h`, MinHook and SEH.
