# tests/linux

Stand-in tests for the native Linux shim. No GPU, game or Steam needed; `run-tests.sh` builds everything
into `build/` (git-ignored) and runs it.

| File | Role |
|---|---|
| `mock_gl.c` | "libSDL2-2.0.so.0": `SDL_GL_GetProcAddress` + a small GL state machine (FBOs, textures, renderbuffers, memory objects, semaphores) that records every call to `$MOCK_TRACE` |
| `fake_togl.cpp`, `decls.h` | stand-in `libtogl.so` with the real C++ signatures (so the mangled names match) and ToGL's lazy FBO binding |
| `fake_game.cpp` | loading frames, then gameplay frames with many `SetRenderTarget(0)` switches ending on the back buffer |
| `mock_helper.c` | helper stand-in: real unix socket + `SCM_RIGHTS`, protocol v3 (`--sem`, `--version N`, `--die-after N`) |
| `client.c` | drives the REAL helper like the shim would |
| `suite.py`, `run_case.sh` | the cases and assertions (call ordering, fallbacks, state restore, trace trigger) |

What it can not show: anything that depends on the real driver (semaphore fd sharing, GL/Vulkan ordering on
the GPU, image content). Those need `bmvr_xfer_helper --selftest-sync` and a real run.
