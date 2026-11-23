# The native Linux build: why it is a different architecture

`AGENTS.md` says to follow L4D2VR's architecture and to document divergences once the failure is verified. These are the
verified facts behind the divergence (from `tools/linux/bmvr-probe*.sh` output on a CachyOS x86_64 machine; native build
files from Steam):

- The native build is **32-bit x86** (`bms_linux`, `bin/launcher.so`, all game libraries).
- It renders through **ToGL** (`bin/libtogl.so`, Direct3D 9 on OpenGL). `launcher.so` and `shaderapidx9.so` reference
  `libtogl.so`; **no binary or script mentions DXVK**. `bin/libdxvk_d3d9.so` ships in the folder but nothing loads it. So the
  DXVK-based L4D2VR path (`IDirect3DVR9`, Vulkan eye textures) is not selectable on this build.
- `libtogl.so` **exports its D3D9 classes by name** (`IDirect3DDevice9::Present`, `SetRenderTarget`, `StretchRect`,
  `CreateTexture`, `CreateRenderTarget`, `Reset`, `IDirect3D9::CreateDevice`, `Direct3DCreate9`, ...) and `shaderapidx9.so`
  imports 123 D3D symbols from it. These can be interposed with `LD_PRELOAD`.
- Game code is **stripped**: `client.so` exports 383 dynamic symbols, the other modules about six. Hook points must be found
  the way the Windows ones were: anchor strings and RTTI. `client.so` still contains anchors such as
  `CViewRender::RenderView`, `CViewRender::ViewDrawScene`, `CViewRender::DrawViewModel`, `CViewRender::SetUpViews`,
  `CClientShadowMgr::UpdateFlashlightState`, and RTTI names (`11CViewRender`, `15C_BaseViewModel`).
- Interface names match the Windows build: `VClient018`, `VEngineClient015`, `VMaterialSystem081`, `VEngineRenderView014`.
- The game's bundled `bin/libopenvr_api.so` is the old (pre-1.0, `VR_Init`) ABI.
- SteamVR on the test machine had no `bin/linux32`. A 32-bit game therefore needs a **64-bit helper** for OpenXR, as on
  Windows.
- The engine loads `libtogl.so` and SDL with `RTLD_LOCAL`, so `dlsym(RTLD_NEXT/RTLD_DEFAULT)` cannot see them. The shim looks
  them up through an explicit `RTLD_NOLOAD` handle (`find_loaded_lib`).

Consequence: the Linux port keeps L4D2VR's *ideas* (two `RenderView`s per frame into eye targets, per-eye off-centre frusta,
pose that travels with the frame, controller input into `CreateMove`) but replaces its transport: ToGL interposition instead of
a DXVK fork, GL -> Vulkan shared memory instead of `IDirect3DVR9`, an OpenXR helper instead of OpenVR in the game process.
See [architecture.md](architecture.md).
