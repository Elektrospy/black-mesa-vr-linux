# tests/linux

Stand-in tests for the native Linux shim. No GPU, game or Steam needed; `run-tests.sh` builds everything
into `build/` (git-ignored) and runs it.

| File | Role |
|---|---|
| `mock_gl.c` | "libSDL2-2.0.so.0": `SDL_GL_GetProcAddress` + a small GL state machine (FBOs, textures, renderbuffers, memory objects, semaphores) that records every call to `$MOCK_TRACE` |
| `fake_togl.cpp`, `decls.h` | stand-in `libtogl.so` with the real C++ signatures (so the mangled names match) and ToGL's lazy FBO binding |
| `fake_game.cpp` | loading frames, then gameplay frames with many `SetRenderTarget(0)` switches ending on the back buffer, then (optional 3rd arg) a second loading screen |
| `mock_helper.c` | helper stand-in: real unix socket + `SCM_RIGHTS`, protocol v4 (`--sem`, `--eyes N`, `--version N`, `--die-after N`, `--die-idle-after N` = exit right after acking frame N) |
| `lock_test.c` | the helper's one-instance `flock()` guard (`linux/bmvr_single_instance.h`), no Vulkan needed |
| `client.c` | drives the REAL helper like the shim would |
| `suite.py`, `run_case.sh` | the cases and assertions (call ordering, fallbacks, state restore, trace trigger) |

What it can not show: anything that depends on the real driver (semaphore fd sharing, GL/Vulkan ordering on
the GPU, image content). Those need `bmvr_xfer_helper --selftest-sync` and a real run.

`test_xr_probe.sh [helper] [--software]` is NOT part of `run-tests.sh` either (stage 4a, X0): it needs an OpenXR loader and Monado installed. It starts its own
headless Monado (simulated HMD, null compositor; refuses to run if a Monado is already running) and checks `bmvr_xfer_helper --xr-probe` (exit codes, summary, 10 repeats).
`--software` uses Mesa's lavapipe for machines without a GPU. It says nothing about SteamVR or real hardware.

`test_kill_loop.sh` is NOT part of `run-tests.sh`: it is a real-game test (needs the game running, a GPU and the real helper). It kills the
helper with `kill -9` in a loop and checks that the shim notices and reconnects (`--eyes 2` for the two-eye case). Run it as a program from any shell.

`BMVR_TEST_NATIVE=1 tests/linux/run-tests.sh` builds everything for the host (64-bit) instead of `-m32`, so it runs without multilib.
It covers all the shim logic (T1-T19) but not the 32-bit build; the plain run is the authoritative one. A build directory deeper than
55 characters is replaced by a temporary one (unix socket paths are limited to 107 characters).
T14-T19 are the stage 3d two-eye cases (`fake_game` takes `FAKE_SKIP_RT` / `FAKE_SKIP_AFTER` to stop rendering one target).
