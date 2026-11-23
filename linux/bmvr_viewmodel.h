/*
 * Black Mesa VR - native Linux port: the weapon viewmodel at the controller.
 *
 * C_BaseViewModel::CalcViewModelView(owner, eyePosition, eyeAngles) places the gun relative to the EYE the engine passes in.
 * The shim feeds it a substitute eye so the gun lands on the weapon controller:
 *
 *   angles   = the controller's AIM pose (the way shots go with input_aim=1) + vm_rot
 *   position = hand position (GRIP pose) - (forward * off[0] + right * off[1] + up * off[2])
 *
 * off[] is where the gun sits relative to the eye in the engine's own viewmodel placement (Source units, forward / right / up), so
 * subtracting it puts the gun, not the eye, on the hand. The Windows build's defaults are 16, 3, -2.
 *
 * Pure math (no engine, no GL), checked by tests/linux/viewmodel_test.c.
 */
#ifndef BMVR_VIEWMODEL_H
#define BMVR_VIEWMODEL_H

#include "bmvr_stereo.h"

/* grip / aim: pos xyz (OpenXR metres) + quat xyzw, as in struct bmvr_xfer_input. game_origin / game_yaw_deg: the game's own camera, the
 * same rule the eye views use. Returns 0 for an unusable pose or when the stereo rig has no head reference yet. */
static inline int bmvr_viewmodel_eye(const struct bmvr_stereo_rig *rig, const float grip[7], const float aim[7], const float game_origin[3],
                                     float game_yaw_deg, const float off[3], const float rot[3], float out_pos[3], float out_ang[3])
{
    float horg[3], hf[3], hr[3], hu[3];
    float aorg[3], af[3], ar[3], au[3];
    if (!bmvr_stereo_pose_world(rig, grip, grip + 3, game_origin, game_yaw_deg, horg, hf, hr, hu)) return 0;
    if (!bmvr_stereo_pose_world(rig, aim, aim + 3, game_origin, game_yaw_deg, aorg, af, ar, au)) return 0;
    const float left[3] = { -ar[0], -ar[1], -ar[2] };
    float ang[3];
    bmvr_basis_to_angles(af, left, au, ang);
    for (int i = 0; i < 3; i++) ang[i] += rot[i];
    float f[3], r[3], u[3];
    bmvr_angle_vectors(ang, f, r, u);
    for (int i = 0; i < 3; i++) {
        out_ang[i] = ang[i];
        out_pos[i] = horg[i] - (f[i] * off[0] + r[i] * off[1] + u[i] * off[2]);
    }
    return 1;
}

#endif
