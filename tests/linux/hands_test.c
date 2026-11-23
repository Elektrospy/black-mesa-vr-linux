/* Stage 5 slice 5: the hand models' CPU half (linux/bmvr_hands.h) against the real VR/hands/NAME.glb files. No GL, no headset.
 *   cc -O2 -Wall -Wextra -std=gnu11 -I linux -o hands_test tests/linux/hands_test.c -lm && ./hands_test <path to VR/hands> */
#include <stdio.h>
#include "bmvr_hands.h"
#include "bmvr_stereo.h"
#include "bmvr_fingers.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int near(float a, float b, float e) { return fabsf(a - b) < e; }

static void skin_vertex(const struct bh_asset *a, float pal[BH_MAXJ][16], int v, float out[3])
{
    const float *vt = a->vert + 16 * (size_t)v;
    out[0] = out[1] = out[2] = 0.0f;
    for (int k = 0; k < 4; k++) {
        const float w = vt[12 + k];
        if (w <= 0.0f) continue;
        const float *m = pal[(int)vt[8 + k]];
        for (int r = 0; r < 3; r++) out[r] += w * (m[r * 4] * vt[0] + m[r * 4 + 1] * vt[1] + m[r * 4 + 2] * vt[2] + m[r * 4 + 3]);
    }
}

static void check_asset(const char *dir, const char *file, int right)
{
    char path[600]; snprintf(path, sizeof path, "%s/%s", dir, file);
    size_t n = 0; uint8_t *buf = bh_read_file(path, &n);
    CHECK(buf != NULL, "%s: readable", file);
    if (!buf) return;
    struct bh_asset a; const char *err = "";
    CHECK(bh_load_glb(&a, buf, n, &err), "%s: loads (%s)", file, err);
    free(buf);
    if (!a.nv) return;
    CHECK(a.nj == 16 && a.nv > 1000 && a.ni % 3 == 0, "%s: 16 joints, a real mesh (nj %d nv %d ni %d)", file, a.nj, a.nv, a.ni);
    CHECK(a.tex && a.tw >= 256 && a.th >= 256, "%s: PNG decoded (%dx%d)", file, a.tw, a.th);
    if (a.tex) { long sum = 0; for (size_t i = 0; i < (size_t)a.tw * a.th * 4; i += 4099) sum += a.tex[i]; CHECK(sum > 0, "%s: the texture is not black", file); }
    CHECK(a.right == right && a.rig_ok, "%s: rig found, %s hand", file, right ? "right" : "left");
    CHECK(a.parent[0] == -1 && a.parent[1] == 0 && a.parent[2] == 1, "%s: joint parents from the node tree", file);
    for (int i = 0; i < a.ni; i++) if (a.idx[i] >= (uint32_t)a.nv) { CHECK(0, "%s: index range", file); break; }
    int bad = 0;
    for (int v = 0; v < a.nv; v++) { const float *vt = a.vert + 16 * (size_t)v; float ws = vt[12] + vt[13] + vt[14] + vt[15]; if (!near(ws, 1.0f, 0.02f) || vt[8] >= a.nj || vt[9] >= a.nj || vt[10] >= a.nj || vt[11] >= a.nj) bad++; }
    CHECK(bad == 0, "%s: weights sum to 1 and joint indices are in range (%d bad)", file, bad);

    /* open hand: the palette is the identity, so the skinned mesh is the bind mesh */
    float pal[BH_MAXJ][16], open[5] = { 0, 0, 0, 0, 0 }, fist[5] = { 0.1f, 1, 1, 1, 1 };
    CHECK(bh_palette(&a, open, pal), "%s: palette", file);
    float id[16]; bh_ident(id);
    int idbad = 0;
    for (int j = 0; j < a.nj; j++) for (int k = 0; k < 16; k++) if (!near(pal[j][k], id[k], 1e-3f)) idbad++;
    CHECK(idbad == 0, "%s: all-open curls give the bind pose (%d off)", file, idbad);

    /* closed fingers: the fingertips move, the wrist-side vertices do not */
    float closed[BH_MAXJ][16];
    CHECK(bh_palette(&a, fist, closed), "%s: fist palette", file);
    float maxmove = 0.0f, minmove = 1e9f, p0[3], p1[3];
    for (int v = 0; v < a.nv; v++) {
        skin_vertex(&a, pal, v, p0); skin_vertex(&a, closed, v, p1);
        float d = sqrtf((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1]) + (p1[2] - p0[2]) * (p1[2] - p0[2]));
        if (d > maxmove) maxmove = d;
        if (d < minmove) minmove = d;
        if (d != d) { CHECK(0, "%s: NaN in the skinned mesh", file); break; }
    }
    CHECK(maxmove > 0.03f && maxmove < 0.3f, "%s: a fist moves the fingertips by centimetres (max %.3f m)", file, (double)maxmove);
    CHECK(minmove < 1e-4f, "%s: the palm stays put (min %.5f m)", file, (double)minmove);
    bh_free(&a);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "VR/hands";
    check_asset(dir, "hev_glove_right_model.glb", 1);
    check_asset(dir, "hev_glove_left_model.glb", 0);
    check_asset(dir, "bare_hand_right_model.glb", 1);
    check_asset(dir, "bare_hand_left_model.glb", 0);

    /* finger curls from tracked joints (the SteamVR skeletal input, as XR_EXT_hand_tracking reports it) */
    {   struct bf_joint j[BF_JOINTS];
        float cu[5];
        const int base[5] = { BF_THUMB_METACARPAL, BF_INDEX_METACARPAL, BF_MIDDLE_METACARPAL, BF_RING_METACARPAL, BF_LITTLE_METACARPAL };
        /* every finger a chain along +X (straight), or turning by `bend` radians per joint (a fist is about 60 deg each) */
        for (int pass = 0; pass < 3; pass++) {
            const float bend = pass == 1 ? 1.0472f : 0.0f;
            memset(j, 0, sizeof j);
            for (int i = 0; i < BF_JOINTS; i++) j[i].valid = 1;
            for (int f = 0; f < 5; f++) {
                float x = 0.0f, y = 0.0f, a = 0.0f;
                for (int k = 0; k < 5; k++) {
                    j[base[f] + k].p[0] = x; j[base[f] + k].p[1] = y; j[base[f] + k].p[2] = (float)f * 0.02f;
                    x += 0.03f * cosf(a); y += 0.03f * sinf(a);
                    if (k > 0 || f > 0) a += bend;
                }
            }
            if (pass == 2) { j[BF_MIDDLE_METACARPAL + 2].valid = 0; j[BF_RING_METACARPAL].valid = 0; }   /* two fingers lose a joint */
            const int n = bf_hand_curls(j, 1, cu);
            if (pass == 0) CHECK(n == 5 && cu[0] < 0.01f && cu[1] < 0.01f && cu[4] < 0.01f, "fingers: a straight hand is open (%d fingers, %.3f %.3f %.3f)", n, cu[0], cu[1], cu[4]);
            if (pass == 1) CHECK(n == 5 && cu[1] > 0.9f && cu[2] > 0.9f && cu[3] > 0.9f && cu[4] > 0.9f, "fingers: joints bent 60 deg each close the finger (%.3f %.3f %.3f %.3f)", cu[1], cu[2], cu[3], cu[4]);
            if (pass == 2) CHECK(n == 3 && cu[2] == 0.0f && cu[3] == 0.0f && cu[1] == 0.0f, "fingers: a finger with an untracked joint is left out (%d usable)", n);
        }
        CHECK(bf_hand_curls(j, 0, cu) == 0 && cu[1] == 0.0f, "fingers: an inactive hand gives nothing");
    }
    /* curls from trigger / grip */
    float c[5];
    bh_curls_from_input(0, 0, c);
    CHECK(near(c[0], 0.1f, 1e-5f) && near(c[1], 0.1f, 1e-5f) && near(c[4], 0.1f, 1e-5f), "curls: resting hand is slightly curled");
    bh_curls_from_input(1, 0, c);
    CHECK(near(c[1], 1.0f, 1e-5f) && near(c[2], 0.1f, 1e-5f), "curls: trigger closes the index only");
    bh_curls_from_input(0, 1, c);
    CHECK(near(c[2], 1.0f, 1e-5f) && near(c[1], 0.35f, 1e-5f), "curls: grip closes middle, ring and pinky, half-closes the index");

    /* a controller in the world uses the same rule as the eyes */
    {   struct bmvr_stereo_rig rig; bmvr_stereo_rig_init(&rig, 40.0f);
        rig.have_ref = 1; rig.ref_yaw = 0.0f; rig.ref_pos[0] = rig.ref_pos[1] = rig.ref_pos[2] = 0.0f;
        const float pos[3] = { 0, 0, -1 }, quat[4] = { 0, 0, 0, 1 }, go[3] = { 100, 200, 300 };
        float o[3], f[3], r[3], u[3];
        CHECK(bmvr_stereo_pose_world(&rig, pos, quat, go, 0.0f, o, f, r, u), "pose_world: usable");
        CHECK(near(o[0], 140.0f, 1e-3f) && near(o[1], 200.0f, 1e-3f) && near(o[2], 300.0f, 1e-3f), "pose_world: 1 m forward = 40 units along +X (%.1f %.1f %.1f)", o[0], o[1], o[2]);
        CHECK(near(f[0], 1.0f, 1e-4f) && near(r[1], -1.0f, 1e-4f) && near(u[2], 1.0f, 1e-4f), "pose_world: identity points along +X, right is -Y, up is +Z");
        bmvr_stereo_pose_world(&rig, pos, quat, go, 90.0f, o, f, r, u);
        CHECK(near(o[0], 100.0f, 1e-2f) && near(o[1], 240.0f, 1e-2f) && near(f[1], 1.0f, 1e-4f), "pose_world: a body turn of 90 deg swings the hand to +Y (%.1f %.1f)", o[0], o[1]);
        rig.have_ref = 0;
        CHECK(!bmvr_stereo_pose_world(&rig, pos, quat, go, 0.0f, o, f, r, u), "pose_world: no reference, no pose");
    }
    /* controller -> world: identity axes (right=-Y, up=+Z, forward=+X in Source), no offsets, scale 1 */
    {   const float o[3] = { 10, 20, 30 }, r[3] = { 0, -1, 0 }, u[3] = { 0, 0, 1 }, f[3] = { 1, 0, 0 }, pm[3] = { 0, 0, 0 }, rd[3] = { 0, 0, 0 };
        float W[16];
        bh_controller_world(o, r, u, f, 1.0f, 1.0f, pm, rd, W);
        /* model -Z (the hand extends forward) must land on +forward */
        float x = W[2] * -1.0f + W[3], y = W[6] * -1.0f + W[7], z = W[10] * -1.0f + W[11];
        CHECK(near(x, 11.0f, 1e-4f) && near(y, 20.0f, 1e-4f) && near(z, 30.0f, 1e-4f), "world: model -Z is the controller's forward (%.2f %.2f %.2f)", x, y, z);
        /* model +X is the controller's right */
        x = W[0] + W[3]; y = W[4] + W[7]; z = W[8] + W[11];
        CHECK(near(x, 10.0f, 1e-4f) && near(y, 19.0f, 1e-4f) && near(z, 30.0f, 1e-4f), "world: model +X is the controller's right (%.2f %.2f %.2f)", x, y, z);
        /* 180 deg about Y turns the hand around: -Z now goes backwards */
        const float ry[3] = { 0, 180, 0 };
        bh_controller_world(o, r, u, f, 1.0f, 1.0f, pm, ry, W);
        x = W[2] * -1.0f + W[3];
        CHECK(near(x, 9.0f, 1e-4f), "world: the Y-180 correction flips the hand around (%.2f)", x);
        /* scale and the metre offset */
        const float po[3] = { 0, 0, -0.1f };
        bh_controller_world(o, r, u, f, 40.0f, 0.5f, po, rd, W);
        x = W[3];
        CHECK(near(x, 10.0f + 0.1f * 40.0f, 1e-3f), "world: a -0.10 m local Z offset moves the hand forward 4 units (%.2f)", x);
    }
    /* projection: a centred frustum maps the view axis to the middle; a point on the right edge of a 90 deg frustum to x = 1 */
    {   const float frac[4] = { 0, 1, 0, 1 };
        float P[16];
        bh_projection(90.0f, 1.0f, frac, 1.0f, 100.0f, 0, P);
        float cx = P[0] * 5.0f + P[2] * 5.0f, cw = 5.0f;
        CHECK(near(cx / cw, 1.0f, 1e-4f), "projection: 45 deg right of the axis is the right edge (%.3f)", cx / cw);
        float zc = P[10] * 1.0f + P[11], zw = 1.0f;
        CHECK(near(zc / zw, -1.0f, 1e-4f), "projection: the near plane is z = -1 (%.3f)", zc / zw);
        bh_projection(90.0f, 1.0f, frac, 1.0f, 100.0f, 1, P);
        CHECK(P[5] < 0.0f, "projection: flip_y negates y");
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
