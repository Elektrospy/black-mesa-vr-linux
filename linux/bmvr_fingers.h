/*
 * Black Mesa VR - native Linux port: finger curls from tracked hand joints (the SteamVR skeletal input, as OpenXR serves it).
 *
 * SteamVR's own skeletal input (Index controllers' finger sensors, Vive wands' touch areas) reaches an OpenXR application through
 * XR_EXT_hand_tracking: the runtime reports the 26 hand joints, driven by the controllers when you hold them. The helper asks for
 * them when the runtime offers the extension, turns them into five curls with the formula of the Windows helper
 * (OpenXRHelper64 ComputeCurl: the sum of the bend angles of a finger's joints, divided by 3.2 rad) and sends them in the input
 * channel. Without the extension, or without tracked joints, nothing is sent and the shim keeps bending the fingers from the
 * trigger and the grip (bh_curls_from_input in bmvr_hands.h).
 *
 * No OpenXR or OpenVR header here: the joint numbers are XrHandJointEXT's, checked against openxr.h by a _Static_assert in
 * bmvr_xr_sink.c.
 */
#ifndef BMVR_FINGERS_H
#define BMVR_FINGERS_H

#include <math.h>
#include <stdint.h>

#define BF_JOINTS 26
/* XrHandJointEXT: palm 0, wrist 1, then metacarpal / proximal / (intermediate) / distal / tip per finger */
#define BF_THUMB_METACARPAL 2
#define BF_INDEX_METACARPAL 6
#define BF_MIDDLE_METACARPAL 11
#define BF_RING_METACARPAL 16
#define BF_LITTLE_METACARPAL 21

struct bf_joint { float p[3]; int valid; };   /* position (any space, any unit) and whether the runtime tracks it */

static inline float bf_angle(const float a[3], const float b[3])
{
    const float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (la * lb <= 1e-6f) return 0.0f;
    float c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c);
}

/* The curl of one chain a-b-c-d-e: 0 = straight, 1 = closed. 0 and *ok = 0 when a joint is untracked. */
static inline float bf_chain(const struct bf_joint *j, int a, int b, int c, int d, int e, int *ok)
{
    if (!j[a].valid || !j[b].valid || !j[c].valid || !j[d].valid || !j[e].valid) { *ok = 0; return 0.0f; }
    float v[4][3];
    const int ix[5] = { a, b, c, d, e };
    for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) v[i][k] = j[ix[i + 1]].p[k] - j[ix[i]].p[k];
    const float curl = (bf_angle(v[1], v[0]) + bf_angle(v[2], v[1]) + bf_angle(v[3], v[2])) / 3.20f;
    *ok = 1;
    return curl < 0.0f ? 0.0f : curl > 1.0f ? 1.0f : curl;
}

/* thumb, index, middle, ring, pinky. Returns the number of fingers that could be computed (0..5); the hand is usable from 4 on.
 * An untracked finger is left at 0. `active` is the runtime's isActive for the hand: an inactive hand gives 0. */
static inline int bf_hand_curls(const struct bf_joint j[BF_JOINTS], int active, float out[5])
{
    int n = 0, ok;
    for (int i = 0; i < 5; i++) out[i] = 0.0f;
    if (!active) return 0;
    const int th = BF_THUMB_METACARPAL;
    out[0] = bf_chain(j, th, th + 1, th + 2, th + 3, th + 3, &ok); n += ok;              /* the thumb has four joints: the last bend is 0 */
    static const int base[4] = { BF_INDEX_METACARPAL, BF_MIDDLE_METACARPAL, BF_RING_METACARPAL, BF_LITTLE_METACARPAL };
    for (int f = 0; f < 4; f++) { out[f + 1] = bf_chain(j, base[f], base[f] + 1, base[f] + 2, base[f] + 3, base[f] + 4, &ok); n += ok; }
    return n;
}

#endif
