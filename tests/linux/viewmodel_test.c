/* The viewmodel placement math (linux/bmvr_viewmodel.h). No GPU, no engine.
 *   cc -O2 -Wall -Wextra -std=gnu11 -I linux -o viewmodel_test tests/linux/viewmodel_test.c -lm && ./viewmodel_test */
#include <stdio.h>
#include "bmvr_viewmodel.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static void make_rig(struct bmvr_stereo_rig *r)
{
    bmvr_stereo_rig_init(r, 39.37f);
    r->have_ref = 1;           /* head at the OpenXR origin, looking straight ahead (yaw 0) */
    r->ref_yaw = 0.0f;
}

int main(void)
{
    struct bmvr_stereo_rig rig;
    make_rig(&rig);
    const float go[3] = { 100.0f, 200.0f, 60.0f };
    const float off[3] = { 16.0f, 3.0f, -2.0f }, rot0[3] = { 0, 0, 0 };
    float pos[3], ang[3];

    /* a controller one metre ahead and 0.25 m right of the head, pointing straight ahead (OpenXR -Z), identity orientation */
    const float grip[7] = { 0.25f, 0.0f, -1.0f, 0, 0, 0, 1 };
    const float aim[7]  = { 0.25f, 0.0f, -1.0f, 0, 0, 0, 1 };
    CHECK(bmvr_viewmodel_eye(&rig, grip, aim, go, 0.0f, off, rot0, pos, ang), "placement succeeds");
    CHECK(near(ang[0], 0, 0.01f) && near(ang[1], 0, 0.01f) && near(ang[2], 0, 0.01f), "angles %.2f %.2f %.2f, expected 0 0 0", ang[0], ang[1], ang[2]);
    /* hand in Source: x = go.x + 39.37, y = go.y - 9.84 (right is -Y), z = go.z. The eye sits off[] behind / left / above it. */
    CHECK(near(pos[0], 100.0f + 39.37f - 16.0f, 0.05f), "x %.2f", pos[0]);
    CHECK(near(pos[1], 200.0f - 9.8425f + 3.0f, 0.05f), "y %.2f (Source right is -Y, so the eye is 3 to the left of the gun-hand line)", pos[1]);
    CHECK(near(pos[2], 60.0f + 2.0f, 0.05f), "z %.2f (the gun sits 2 below the eye, so the eye is 2 above the hand)", pos[2]);

    /* turning the game yaw by 90 degrees (left turn) turns the whole arrangement about the game origin */
    CHECK(bmvr_viewmodel_eye(&rig, grip, aim, go, 90.0f, off, rot0, pos, ang), "placement succeeds at yaw 90");
    CHECK(near(ang[1], 90.0f, 0.01f), "yaw %.2f, expected 90", ang[1]);

    /* the aim pose decides the angles, the grip pose the position: aim pitched up 30 degrees about OpenXR X */
    float q[4]; { const float h = 30.0f * BMVR_PI_F / 360.0f; q[0] = sinf(h); q[1] = 0; q[2] = 0; q[3] = cosf(h); }
    const float aim_up[7] = { 0.25f, 0.0f, -1.0f, q[0], q[1], q[2], q[3] };
    CHECK(bmvr_viewmodel_eye(&rig, grip, aim_up, go, 0.0f, off, rot0, pos, ang), "placement succeeds with a pitched aim");
    CHECK(near(ang[0], -30.0f, 0.05f), "pitch %.2f, expected -30 (up is negative in Source)", ang[0]);

    /* vm_rot adds to the angles */
    const float rot[3] = { 5.0f, -10.0f, 0.0f };
    CHECK(bmvr_viewmodel_eye(&rig, grip, aim, go, 0.0f, off, rot, pos, ang), "placement succeeds with vm_rot");
    CHECK(near(ang[0], 5.0f, 0.01f) && near(ang[1], -10.0f, 0.01f), "rot applied: %.2f %.2f", ang[0], ang[1]);

    /* unusable input */
    const float bad[7] = { 0, 0, 0, 0, 0, 0, 0 };
    CHECK(!bmvr_viewmodel_eye(&rig, bad, aim, go, 0.0f, off, rot0, pos, ang), "a zero quaternion is refused");
    struct bmvr_stereo_rig none; bmvr_stereo_rig_init(&none, 39.37f);
    CHECK(!bmvr_viewmodel_eye(&none, grip, aim, go, 0.0f, off, rot0, pos, ang), "no head reference is refused");

    /* the Windows-style tuned placement: with no tuning it is the aim pose minus the offset along the aim basis */
    {
        const float tune0[3] = { 0, 0, 0 };
        float p2[3], a2[3];
        CHECK(bmvr_viewmodel_eye_tuned(&rig, aim, go, 0.0f, off, tune0, rot0, p2, a2), "tuned placement succeeds");
        CHECK(near(a2[0], 0, 0.01f) && near(a2[1], 0, 0.01f) && near(a2[2], 0, 0.01f), "tuned angles %.2f %.2f %.2f, expected 0 0 0", a2[0], a2[1], a2[2]);
        CHECK(near(p2[0], 100.0f + 39.37f - 16.0f, 0.05f) && near(p2[1], 200.0f - 9.8425f + 3.0f, 0.05f) && near(p2[2], 62.0f, 0.05f), "tuned position %.2f %.2f %.2f", p2[0], p2[1], p2[2]);
        const float tune_y[3] = { 0, 10.0f, 0 };      /* ay 10: yaw 10 degrees to the left about up */
        CHECK(bmvr_viewmodel_eye_tuned(&rig, aim, go, 0.0f, off, tune_y, rot0, p2, a2) && near(a2[1], 10.0f, 0.05f), "ay 10 turns the yaw by 10 (got %.2f)", a2[1]);
        const float tune_x[3] = { 10.0f, 0, 0 };      /* ax 10: muzzle up = pitch -10 in Source */
        CHECK(bmvr_viewmodel_eye_tuned(&rig, aim, go, 0.0f, off, tune_x, rot0, p2, a2) && near(a2[0], -10.0f, 0.05f), "ax 10 pitches the muzzle up (got %.2f)", a2[0]);
    }
    printf("viewmodel_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
