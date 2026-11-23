# Wire protocol (shim <-> helper), versions 1-7

Defined in `linux/bmvr_xfer.h`, current `BMVR_XFER_VERSION` is **7**. Shim and helper must be rebuilt together: a peer with another
version is rejected, with both versions named in the log (`build-linux.sh` rebuilds both). Messages travel over the unix socket
`<game folder>/bmvr_xfer.sock`; image memory and semaphores travel as fds (`SCM_RIGHTS`). The socket is strictly request/response:
the shim only ever blocks in `recv` for an ack.

All structs use fixed-width fields only with `_Static_assert` on their sizes, so the layout is identical in the 32-bit shim and the
64-bit helper (`memory_size` sits at offset 40 after an explicit `reserved`, already 8-aligned).

| Version | Added | Stage |
|---|---|---|
| 1 | hello (helper's image memory fd), frame, ack; window snapshot | 2a |
| 2 | frame carries target label, GL object name, flip flag | 2b |
| 3 | `BMVR_XFER_CAP_SEMAPHORES`: hello carries 3 fds (memory, `gl_ready`, `vk_done`); `BMVR_XFER_FLAG_SEMAPHORE`; ack = "submission done" | 3a |
| 4 | two eyes: hello 40 -> 48 bytes (`eye_count`, `reserved`), one memory fd per image then the two semaphore fds; frame 48 -> 92 bytes (`eye_count`, `eye1_src_w/h`, `eye1_flags`, `eye1_gl_name`, `eye1_name`) | 3d |
| 5 | pose channel: `BMVR_XFER_CAP_POSES` (2); every ack carries the helper's newest pose (`struct bmvr_xfer_pose`: `pose_id`, flags `POSE_LOCAL`, `display_time`, `eye[2]{pos[3], rot[4], fov[4]}`; ack 112 bytes); frame carries `pose_id` (96 bytes) | 4a X4 |
| 6 | controller input: `BMVR_XFER_CAP_INPUT` (4); every ack carries `struct bmvr_xfer_input` (160 bytes; ack 272 bytes): per hand stick, trigger, squeeze, 5 buttons (primary = right a / left x, secondary = b / y, menu, stick click, bumper), aim and grip poses in the same LOCAL space as the eye poses, flags (focused, poses valid per hand) | 5 |
| 7 | finger curls: `finger[2][5]` appended (input 200 bytes, ack 312); `BMVR_XFER_INPUT_FINGERS_L / _R` say per hand whether the curls are valid | 5b |

Notes:

- **Deviation from the first design (Decision 5 in [openxr-helper.md](openxr-helper.md)):** there is no separate helper -> shim
  message. Every ack carries the newest pose and the controller state, so poses and input reach the game at the delivery rate
  (`every=N`).
- A frame with `eye_count == 1` on a two-image session touches image 0 only. A hello with `eye_count` outside 1..2 or a frame whose
  `eye_count` exceeds what the helper offered is rejected.
- `BMVR_XFER_FLAG_SEMAPHORE` applies to the whole pair; `FLIPPED` / `WINDOW` are per eye (`eye1_flags`).
- The shim stamps a frame with the pose it was *rendered* with. `xfer_tick` latches ids one Present apart; a pair delivered from the
  stereo hook carries the pose recorded when its eyes were drawn. A pair the hook did not draw carries pose id 0.
- Input values are zero when the OpenXR session is not focused.
