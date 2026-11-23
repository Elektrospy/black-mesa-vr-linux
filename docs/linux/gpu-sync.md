# GPU synchronisation, loading frames, helper death, single helper (stages 3a-3c)

## Stage 3a: GPU semaphores instead of `glFinish` (opt-in `sync=sem`; verified in the real game)

`glFinish` waits for the whole GPU queue of the game. Stage 3a orders the two APIs on the GPU instead:

    GL (shim):   wait(vk_done) [not before the first frame] -> blit -> signal(gl_ready) -> glFlush
    helper:      wait(gl_ready) -> copy the image -> signal(vk_done)

- **Protocol v3.** If the helper offers `BMVR_XFER_CAP_SEMAPHORES` the hello carries three fds: image memory and two
  `VK_KHR_external_semaphore_fd` opaque-fd binary semaphores (a fresh pair per game connection), imported with `GL_EXT_semaphore_fd`.
  Frames carry `BMVR_XFER_FLAG_SEMAPHORE`; the ack then means "queue submission done" (the helper's signal is pending, which is what
  makes the game's next GPU wait legal), not "readback finished".
- **Opt-in:** `sync=sem` or `BMVR_SYNC=sem`. Default stays `sync=finish`. The shim falls back to `glFinish` + ack, and logs why,
  when the helper offers no semaphores, GL lacks the entry points, or the driver rejects the import.
- **Measuring:** every delivered snapshot logs `GL side N us` and `helper round trip N us` with running averages.
- **Helper self-test:** `bmvr_xfer_helper --selftest-sync` (also run by `scripts/build-linux.sh`) checks that the GPU can export and
  import opaque-fd semaphores and runs 50 frame cycles with the helper playing the GL side. No game needed.
- **Risk (why it is opt-in):** a GPU wait on a semaphore whose signal never arrives has no timeout. The helper detects its half (5 s
  fence timeout, then exits with an explanation). The shim's guard is stage 3b.

### Real-game result (RTX 3080, driver 620.70.06)

- The driver exports and imports the semaphores (`opaque-fd semaphore: extension=1 exportable=1 importable=1`), the shim imported both
  (`GL semaphores 1/2 imported`), and `sync=sem` delivered every armed gameplay frame.
- Time in the game's render thread per delivered snapshot: `sync=finish` 2.0-4.4 ms (average 3.05 ms over 10), `sync=sem` 79-112 us.
  The `sync=finish` helper round trip (about 1.07 s) is dominated by writing a PPM and is not comparable.
- Snapshots upright with `flip=0`, consecutive gameplay frames consistent, no overlay, no visible tearing at half size
  (single-scanline tearing not checked pixel by pixel).
- The "no matching render target was left in the armed frame" lines fell on loading frames; the counters in that line were never
  reset (fixed in 3b).

### Stand-in coverage

Default path unchanged; `sync=sem` imports both semaphores as OPAQUE_FD, per frame exactly `[wait] blit signal flush`, no `glFinish`,
no wait before the first frame, each wait only after the previous ack, barrier = the shared texture in layout GENERAL; every
fallback; protocol version mismatch; helper dying mid-session; renderbuffer and texture targets and the window source. The real
helper ran on Mesa's software Vulkan with the Khronos validation layer (v3 hello, sanitised snapshot names, a semaphore frame it never
offered is rejected, selftest silent). Software Vulkan cannot export semaphores, so the fd path of the self-test only ran on the
real GPU.

## Stage 3b: loading frames and a hung-GPU guard

- **Loading / menu frames.** If the armed frame made 0 `SetRenderTarget(0)` calls the shim re-checks the last delivered object
  (`glIsTexture` / `glIsRenderbuffer`, then size, single-sample) and delivers that. Log: `loading/menu frame ... delivering the last
  known object of 'X'`. A gone or resized object is forgotten. A loading frame before any gameplay frame is silent.
  Assumption still to confirm in the real game: the back buffer's GL object holds the loading-screen picture at the end of those frames.
- **Helper death before the GPU wait.** Before each snapshot the shim peeks the helper socket (`MSG_PEEK | MSG_DONTWAIT`). A closed
  socket tears everything down and goes back to searching, without issuing the wait. Not covered: a helper that is alive but whose
  GPU work never finishes (its own 5 s fence timeout handles that side).
- **Diagnostics.** The armed-frame counters are reset when a frame is armed, and the "no matching render target" line prints the
  frame's `SetRenderTarget(0)` count.

Tests: T12 (helper exits right after acking: no `glWaitSemaphoreEXT` / blit / signal afterwards, semaphores deleted), T13 (loading
screens before and after gameplay).

### Real-game kill loop (verified 2026-11-19, and again with two eyes)

`tests/linux/test_kill_loop.sh` starts a helper, waits for `xfer: helper connected`, waits 0-5 s at random, `kill -9`s the helper and
waits for the shim to log `lost the helper` / `helper's socket is closed` / `back to searching`. The next cycle's connect line
proves the reconnect. Run it as a program, not pasted or `source`d (fish).

- Black Mesa native, RTX 3080, `target=InternalRT0`, `flip=0`, `sync=sem`: **10/10** (interrupted by hand; default is 30). With
  `--eyes 2`: 10/10, then the default **30/30**. No GPU hang, no crash.
- The first attempt failed because an earlier helper still held the game's connection. The test now refuses to start while a helper
  runs (`pgrep -f`; `pgrep -x` cannot be used, the process name is 16 characters and the kernel keeps 15), and 3c fixes the cause.
- The shim retries every `XFER_RETRY` = 240 presents (about 4 s at 60 fps), so the script waits up to 45 s for a reconnect.
- Ten or thirty random kill moments are a sample, not a proof.

## Stage 3c: one helper per game folder

`bmvr_xfer_helper` takes an exclusive non-blocking `flock()` on `<game folder>/bmvr_xfer.lock` **before** it touches the GPU and
exits naming the holder's pid when another helper already serves that folder. The kernel drops the lock when the process dies
(including `kill -9`), so there is no stale-lock cleanup. Code: `linux/bmvr_single_instance.h` (header-only; the test uses the same
function). `--selftest-sync` does not lock. Helpers from different game folders do not interfere. A pre-3c helper binary does not
take the lock: after upgrading, make sure no old helper is running.

Tests: `tests/linux/lock_test.c` (second holder refused with pid, lock free after `kill -9`, unwritable folder gives a clear error).

**Still to do against the real helper** (the file needs the Vulkan headers, which were not available where it was written): start
one helper, start a second in another terminal; the second must print `ERROR: another bmvr_xfer_helper already serves this game
folder ... (pid N)` and exit 1, the first must keep running. (The XR sink's stand-in run showed "second helper, same folder: refused",
see [openxr-results.md](openxr-results.md).)
