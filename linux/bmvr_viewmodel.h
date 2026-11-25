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

/* Rodrigues rotation of v about the unit vector k (the Windows build's VectorRotate). */
static inline void bmvr_vec_rotate(const float v[3], const float k[3], float deg, float o[3])
{
    const float r = deg * BMVR_PI_F / 180.0f, c = cosf(r), s = sinf(r);
    const float cr[3] = { k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0] };
    const float d = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    for (int i = 0; i < 3; i++) o[i] = v[i] * c + cr[i] * s + k[i] * d * (1.0f - c);
}

/* The Windows build's weapon placement (VR::GetRecommendedViewmodelAbsPos / ApplyViewmodelBasisOffsets), for Touch-style controllers and the Steam Frame:
 * the AIM pose gives both the position and the basis; the per-weapon angle offsets tune[] = (ax, ay, az) turn the basis (ay about up, ax about right, az about
 * forward, in that order); the eye is placed off[] (forward, right, up: ox + the Touch extra, oy, oz) back from the controller along the turned basis.
 * rot[] (pitch, yaw, roll) is an extra on top, as in bmvr_viewmodel_eye(). */
static inline int bmvr_viewmodel_eye_tuned(const struct bmvr_stereo_rig *rig, const float aim[7], const float game_origin[3], float game_yaw_deg,
                                           const float off[3], const float tune[3], const float rot[3], float out_pos[3], float out_ang[3])
{
    float aorg[3], f[3], r[3], u[3], t[3];
    if (!bmvr_stereo_pose_world(rig, aim, aim + 3, game_origin, game_yaw_deg, aorg, f, r, u)) return 0;
    bmvr_vec_rotate(f, u, tune[1], t); for (int i = 0; i < 3; i++) f[i] = t[i];
    bmvr_vec_rotate(r, u, tune[1], t); for (int i = 0; i < 3; i++) r[i] = t[i];
    bmvr_vec_rotate(f, r, tune[0], t); for (int i = 0; i < 3; i++) f[i] = t[i];
    bmvr_vec_rotate(u, r, tune[0], t); for (int i = 0; i < 3; i++) u[i] = t[i];
    bmvr_vec_rotate(r, f, tune[2], t); for (int i = 0; i < 3; i++) r[i] = t[i];
    bmvr_vec_rotate(u, f, tune[2], t); for (int i = 0; i < 3; i++) u[i] = t[i];
    const float left[3] = { -r[0], -r[1], -r[2] };
    float ang[3];
    bmvr_basis_to_angles(f, left, u, ang);
    if (rot[0] != 0.0f || rot[1] != 0.0f || rot[2] != 0.0f) {
        for (int i = 0; i < 3; i++) ang[i] += rot[i];
        bmvr_angle_vectors(ang, f, r, u);
    }
    for (int i = 0; i < 3; i++) {
        out_ang[i] = ang[i];
        out_pos[i] = aorg[i] - (f[i] * off[0] + r[i] * off[1] + u[i] * off[2]);
    }
    return 1;
}

#endif
