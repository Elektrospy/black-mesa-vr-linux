# Testing the Linux port

Per-file detail: `tests/linux/README.md`. This page is the map.

## Layers

| Layer | Command | Needs | Shows | Does not show |
|---|---|---|---|---|
| Stand-in suite (T1-T40) | `tests/linux/run-tests.sh` | gcc with 32-bit support, python3; optional Vulkan headers | all shim logic against a fake `libtogl`, mock GL and mock helper: call ordering, fallbacks, state restore, negotiation, hooks refusing wrong guesses | real driver behaviour, image content, real `client.so` |
| Host-native variant | `BMVR_TEST_NATIVE=1 tests/linux/run-tests.sh` | 64-bit toolchain only | everything except the 32-bit build (skips T40 probing) | the 32-bit build; **the plain run is authoritative** |
| Unit tests (run by the suite) | `lock_test`, `stereo_test`, `rtti_test`, `inline_test`, `input_map_test`, `hands_test`, `pose_test`, `viewmodel_test`, `probe_test` | none / `-m32` for `probe_test` | pure maths and data structures | integration |
| Ring stress | `cc -O2 -pthread -I linux -o ring_test tests/linux/ring_test.c` (also `-fsanitize=thread`) | none | no half pair, displayed slot never rewritten, mutation-checked | real GPU timing |
| OpenXR on Monado | `test_xr_probe.sh [--software]`, `test_xr_selftest.sh`, `test_xr_sink.sh [build dir] [--software] [--kill-rounds N]` | OpenXR loader, Monado (+ lavapipe without a GPU) | X0-X2 mechanics, frame integrity, kill loop | SteamVR, the real game, GL, semaphores, a displayed picture |
| Helper self-test | `bmvr_xfer_helper --selftest-sync` | a real GPU | semaphore export/import + 50 cycles | the game |
| Real-game kill loop | `tests/linux/test_kill_loop.sh [-n N] [--eyes 2]` | game running, GPU, real helper | game survives `kill -9` of the helper at random moments and reconnects | |

## Case index (`suite.py`)

T1-T13 single-eye transport (sync modes, fallbacks, protocol mismatch, helper death, registry churn, loading frames); T14-T19 two-eye pairs
([eye-pairs.md](eye-pairs.md)); T20 `every=N`; T21-T23 poses; T23b/c input channel; T24-T31 stereo hook; T32-T36 `CreateMove` hook; T37 hands
without shader entry points; T38-T40 viewmodel hook and slot probe.

## Conventions

- `run-tests.sh` builds into `tests/linux/build` (git-ignored); a build directory deeper than 55 characters is replaced by a temporary one
  (unix socket paths are limited to 107 characters).
- The mock helper speaks the current protocol (`mock_helper --eyes N --poses --input --sem --version N --die-after N --die-idle-after N`).
- `fake_game` options: `FAKE_SKIP_RT`, `FAKE_SKIP_AFTER`, `FAKE_STEREO=1`, `FAKE_BAD_VIEW=1`; `PRE_FLAG=1` pre-creates `bmvr_in_stereo.flag`.
- The tests use the same header-only code as the shim and helper wherever possible (`bmvr_single_instance.h`, `bmvr_pair_ring.h`,
  `bmvr_pose_store.h`, `bmvr_stereo.h`, ...).

## Last known results (as recorded; re-run to confirm)

Host-native suite passes T1-T36 with 139+ checks before stage 5; unit tests: stereo 39, rtti 27, inline 12, input_map 59, hands 67. The
32-bit run on the author's machine passed lock test, T1-T19 and the real-helper section (2026-11-20). Slices 5b and 6 (T37-T40, viewmodel,
probe) are described as stand-in tested in the source notes; the 32-bit-only parts (T40, `probe_test`) have not been recorded as run.
