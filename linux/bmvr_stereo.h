/*
 * Black Mesa VR - native Linux port, stage 4b: stereo views.
 * Design: docs/LINUX_PORT.md, "Stage 4b".
 *
 * Pure math, no engine, no GL, no libc beyond <math.h> (same style as bmvr_pose_store.h, so tests/linux/stereo_test.c can
 * check every convention without a game): turns the helper's OpenXR eye poses (struct bmvr_xfer_pose, v5) into the values the
 * Source engine wants in a CViewSetup: origin, angles (pitch, yaw, roll in degrees), fov, aspect ratio and the off-centre
 * sub-frustum.
 *
 * Conventions (all pinned by tests/linux/stereo_test.c):
 *   OpenXR : right-handed, +X right, +Y up, -Z forward, metres, quaternion (x, y, z, w).
 *   Source : +X forward, +Y LEFT, +Z up, "units" (scale units per metre, default 39.37 = the Windows build's VR scale),
 *            QAngle (pitch, yaw, roll) in degrees: positive pitch looks DOWN, positive yaw turns LEFT.
 *   Axis map OpenXR -> Source: (x, y, z) -> (-z, -x, y). A proper rotation (determinant +1), so orientations map directly.
 *
 * The off-centre mapping is the one the Windows build already uses and that was verified in a headset
 * (src/vr.cpp VR::EyeOffCenterView, src/sdk/sdk.h CViewSetup::m_bOffCenter): the engine builds the projection from
 * fov + aspect + four 0..1 fractions of the CENTRED frustum (0 = left/bottom edge, 1 = right/top edge); we use the smallest
 * centred frustum that holds the eye's rectangle.
 *
 * Head reference ("recenter"): the room is mapped onto the game like this. At a recenter the head's position and yaw are
 * remembered. From then on an eye is placed at (the game's own camera origin) + (the eye's offset from the remembered head
 * position, rotated by the difference between the game's yaw and the remembered head yaw). Orientation is the headset's own
 * (pitch and roll as tracked; yaw = game yaw + head yaw change since the recenter). So walking, crouching and mouse/stick
 * turning stay the game's business, and room-scale motion and head rotation are the headset's.
 */
#ifndef BMVR_STEREO_H
#define BMVR_STEREO_H

#include <math.h>
#include <string.h>
#include "bmvr_xfer.h"

#define BMVR_STEREO_DEFAULT_SCALE 39.37f
#define BMVR_PI_F 3.14159265358979323846f

/* What goes into one eye's CViewSetup (everything else is left as the game had it). */
struct bmvr_view {
    float origin[3];
    float angles[3];      /* pitch, yaw, roll (degrees) */
    float fov_deg;        /* horizontal field of view of the CENTRED frustum the fractions refer to */
    float aspect;         /* that frustum's width / height */
    float frac[4];        /* off-centre: left, right, bottom, top (0..1 of the centred frustum) */
};

struct bmvr_stereo_rig {
    int   have_ref;       /* a head reference exists */
    float ref_pos[3];     /* head position at the recenter, Source units, OpenXR LOCAL-space origin */
    float ref_yaw;        /* head yaw at the recenter, degrees */
    float scale;          /* Source units per metre */
};

static inline void bmvr_v3_cross(const float a[3], const float b[3], float o[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

/* Rotate v by the unit quaternion q = (x, y, z, w). */
static inline void bmvr_quat_rotate(const float q[4], const float v[3], float o[3])
{
    float t[3], c[3];
    const float qv[3] = { q[0], q[1], q[2] };
    bmvr_v3_cross(qv, v, t);
    t[0] *= 2.0f; t[1] *= 2.0f; t[2] *= 2.0f;
    bmvr_v3_cross(qv, t, c);
    o[0] = v[0] + q[3] * t[0] + c[0];
    o[1] = v[1] + q[3] * t[1] + c[1];
    o[2] = v[2] + q[3] * t[2] + c[2];
}

/* OpenXR -> Source axes (no scaling). */
static inline void bmvr_xr_to_src(const float v[3], float o[3])
{
    const float x = v[0], y = v[1], z = v[2];
    o[0] = -z;
    o[1] = -x;
    o[2] = y;
}

/* Rotate a Source-space vector about +Z by `deg` degrees (positive = towards +Y, i.e. a left turn). */
static inline void bmvr_rot_z(const float v[3], float deg, float o[3])
{
    const float r = deg * BMVR_PI_F / 180.0f, c = cosf(r), s = sinf(r);
    const float x = v[0], y = v[1];
    o[0] = c * x - s * y;
    o[1] = s * x + c * y;
    o[2] = v[2];
}

/* Source's AngleVectors(): forward, right, up of (pitch, yaw, roll). Used by the tests and by the basis conversion below. */
static inline void bmvr_angle_vectors(const float ang[3], float f[3], float r[3], float u[3])
{
    const float d = BMVR_PI_F / 180.0f;
    const float sp = sinf(ang[0] * d), cp = cosf(ang[0] * d);
    const float sy = sinf(ang[1] * d), cy = cosf(ang[1] * d);
    const float sr = sinf(ang[2] * d), cr = cosf(ang[2] * d);
    f[0] = cp * cy;                       f[1] = cp * sy;                       f[2] = -sp;
    r[0] = -sr * sp * cy + cr * sy;       r[1] = -sr * sp * sy - cr * cy;       r[2] = -sr * cp;
    u[0] = cr * sp * cy + sr * sy;        u[1] = cr * sp * sy - sr * cy;        u[2] = cr * cp;
}

/* Source's MatrixAngles() for the basis (forward, left, up). `left` is minus AngleVectors' right. */
static inline void bmvr_basis_to_angles(const float f[3], const float l[3], const float u[3], float ang[3])
{
    const float xy = sqrtf(f[0] * f[0] + f[1] * f[1]);
    const float k = 180.0f / BMVR_PI_F;
    if (xy > 0.001f) {
        ang[1] = atan2f(f[1], f[0]) * k;
        ang[0] = atan2f(-f[2], xy) * k;
        ang[2] = atan2f(l[2], u[2]) * k;
    } else {    /* looking straight up or down: yaw from the left vector, roll folded into it */
        ang[1] = atan2f(-l[0], l[1]) * k;
        ang[0] = atan2f(-f[2], xy) * k;
        ang[2] = 0.0f;
    }
}

/* The unit quaternion check: 1 if q is usable (it is normalised into *out when close to unit length). */
static inline int bmvr_quat_ok(const float q[4], float out[4])
{
    const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(n2 > 0.81f && n2 < 1.21f)) return 0;    /* NaN also fails */
    const float inv = 1.0f / sqrtf(n2);
    for (int i = 0; i < 4; i++) out[i] = q[i] * inv;
    return 1;
}

/* An eye's orientation as a Source basis (forward, left, up). */
static inline void bmvr_eye_basis(const float q[4], float f[3], float l[3], float u[3])
{
    const float fx[3] = { 0.0f, 0.0f, -1.0f }, lx[3] = { -1.0f, 0.0f, 0.0f }, ux[3] = { 0.0f, 1.0f, 0.0f };
    float t[3];
    bmvr_quat_rotate(q, fx, t); bmvr_xr_to_src(t, f);
    bmvr_quat_rotate(q, lx, t); bmvr_xr_to_src(t, l);
    bmvr_quat_rotate(q, ux, t); bmvr_xr_to_src(t, u);
}

/* OpenXR XrFovf -> engine fov/aspect/fractions (see the header comment). Returns 0 for a degenerate field of view. */
static inline int bmvr_eye_frustum(const float fov[4], float *fov_deg, float *aspect, float frac[4])
{
    const float l = tanf(fov[0]), r = tanf(fov[1]), u = tanf(fov[2]), d = tanf(fov[3]);
    if (!(r > l && u > d)) return 0;
    const float ux = (-l > r) ? -l : r;
    const float uy = (-d > u) ? -d : u;
    if (!(ux > 0.001f && uy > 0.001f && ux < 20.0f && uy < 20.0f)) return 0;
    *fov_deg = 2.0f * atanf(ux) * 180.0f / BMVR_PI_F;
    *aspect = ux / uy;
    frac[0] = 0.5f + 0.5f * l / ux;
    frac[1] = 0.5f + 0.5f * r / ux;
    frac[2] = 0.5f + 0.5f * d / uy;
    frac[3] = 0.5f + 0.5f * u / uy;
    return 1;
}

/* The head: the midpoint of the eyes (Source units) and the mean viewing direction (unit, Source). 0 when a pose is unusable. */
static inline int bmvr_stereo_head(const struct bmvr_xfer_pose *p, float scale, float pos[3], float fwd[3])
{
    float q[2][4], f[2][3], l[3], u[3];
    for (int e = 0; e < 2; e++) {
        if (!bmvr_quat_ok(p->eye[e].rot, q[e])) return 0;
        bmvr_eye_basis(q[e], f[e], l, u);
    }
    for (int i = 0; i < 3; i++) fwd[i] = f[0][i] + f[1][i];
    const float n = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (!(n > 0.1f)) return 0;
    for (int i = 0; i < 3; i++) fwd[i] /= n;
    float mid[3];
    for (int i = 0; i < 3; i++) mid[i] = 0.5f * (p->eye[0].pos[i] + p->eye[1].pos[i]);
    bmvr_xr_to_src(mid, pos);
    for (int i = 0; i < 3; i++) pos[i] *= scale;
    return 1;
}

static inline void bmvr_stereo_rig_init(struct bmvr_stereo_rig *r, float scale)
{
    memset(r, 0, sizeof *r);
    r->scale = scale > 1.0f ? scale : BMVR_STEREO_DEFAULT_SCALE;
}

/* Remember where the head is now as "straight ahead, at the game's camera". 1 on success. */
static inline int bmvr_stereo_recenter(struct bmvr_stereo_rig *r, const struct bmvr_xfer_pose *p)
{
    float pos[3], fwd[3];
    if (!bmvr_stereo_head(p, r->scale, pos, fwd)) return 0;
    for (int i = 0; i < 3; i++) r->ref_pos[i] = pos[i];
    r->ref_yaw = atan2f(fwd[1], fwd[0]) * 180.0f / BMVR_PI_F;
    r->have_ref = 1;
    return 1;
}

/*
 * The two eye views for pose p, given the game's own camera (origin and yaw of the mono CViewSetup). The first call
 * (no reference yet) recenters on p. Returns 0 (and leaves out[] unspecified) for an unusable pose, in which case the caller
 * draws the game's own view instead.
 */
static inline int bmvr_stereo_eyes(struct bmvr_stereo_rig *r, const struct bmvr_xfer_pose *p, const float game_origin[3],
                                   float game_yaw_deg, struct bmvr_view out[2])
{
    if (!r->have_ref && !bmvr_stereo_recenter(r, p)) return 0;
    const float turn = game_yaw_deg - r->ref_yaw;
    for (int e = 0; e < 2; e++) {
        float q[4];
        if (!bmvr_quat_ok(p->eye[e].rot, q)) return 0;
        float pos[3], rel[3], rot[3];
        bmvr_xr_to_src(p->eye[e].pos, pos);
        for (int i = 0; i < 3; i++) rel[i] = pos[i] * r->scale - r->ref_pos[i];
        bmvr_rot_z(rel, turn, rot);
        for (int i = 0; i < 3; i++) out[e].origin[i] = game_origin[i] + rot[i];

        float f[3], l[3], u[3], fr[3], lr[3], ur[3];
        bmvr_eye_basis(q, f, l, u);
        bmvr_rot_z(f, turn, fr);
        bmvr_rot_z(l, turn, lr);
        bmvr_rot_z(u, turn, ur);
        bmvr_basis_to_angles(fr, lr, ur, out[e].angles);

        if (!bmvr_eye_frustum(p->eye[e].fov, &out[e].fov_deg, &out[e].aspect, out[e].frac)) return 0;
    }
    return 1;
}

#endif
