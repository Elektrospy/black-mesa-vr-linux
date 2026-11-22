/* Stage 4b: the OpenXR -> Source view math (linux/bmvr_stereo.h). No GPU, no engine.
 *   cc -O2 -Wall -Wextra -std=gnu11 -I linux -o stereo_test tests/linux/stereo_test.c -lm && ./stereo_test
 * Every convention is checked against something that does not come from the code under test: Source's own AngleVectors()
 * formulas, hand-worked geometric cases (look left, look up, step forward), and a rebuild of the frustum the engine makes
 * from fov + aspect + the off-centre fractions. */
#include <stdio.h>
#include <stdlib.h>
#include "bmvr_stereo.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }
static float wrap(float a) { while (a > 180.0f) a -= 360.0f; while (a < -180.0f) a += 360.0f; return a; }

static void quat_axis(float deg, float x, float y, float z, float q[4])
{
    const float h = deg * BMVR_PI_F / 360.0f, s = sinf(h);
    q[0] = x * s; q[1] = y * s; q[2] = z * s; q[3] = cosf(h);
}
static void quat_mul(const float a[4], const float b[4], float o[4])   /* a * b */
{
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/* a symmetric 90-degree-ish field of view */
static void set_fov(struct bmvr_xfer_eye_pose *e, float l, float r, float u, float d)
{ e->fov[0] = l; e->fov[1] = r; e->fov[2] = u; e->fov[3] = d; }

static struct bmvr_xfer_pose make_pose(const float q[4], const float head[3])
{
    struct bmvr_xfer_pose p;
    memset(&p, 0, sizeof p);
    p.pose_id = 1; p.flags = BMVR_XFER_POSE_LOCAL;
    for (int e = 0; e < 2; e++) {
        /* eyes are 64 mm apart along the head's own right axis: -X is the left eye in the head frame */
        const float off[3] = { e ? 0.032f : -0.032f, 0.0f, 0.0f };
        float r[3];
        bmvr_quat_rotate(q, off, r);
        for (int i = 0; i < 3; i++) { p.eye[e].pos[i] = head[i] + r[i]; p.eye[e].rot[i] = q[i]; }
        p.eye[e].rot[3] = q[3];
        set_fov(&p.eye[e], -0.8f, 0.8f, 0.8f, -0.8f);
    }
    return p;
}

int main(void)
{
    const float S = BMVR_STEREO_DEFAULT_SCALE;
    const float ident[4] = { 0, 0, 0, 1 };
    const float head0[3] = { 0.1f, 1.7f, 0.2f };

    /* 1. angles -> AngleVectors -> basis -> angles round-trips (the conversion is Source's MatrixAngles) */
    {
        int bad = 0;
        srand(7);
        for (int i = 0; i < 2000; i++) {
            float a[3] = { (float)(rand() % 17800 - 8900) / 100.0f, (float)(rand() % 35800 - 17900) / 100.0f, (float)(rand() % 35800 - 17900) / 100.0f };
            float f[3], r[3], u[3], l[3], b[3];
            bmvr_angle_vectors(a, f, r, u);
            for (int k = 0; k < 3; k++) l[k] = -r[k];
            bmvr_basis_to_angles(f, l, u, b);
            if (!near(wrap(b[0] - a[0]), 0, 0.02f) || !near(wrap(b[1] - a[1]), 0, 0.02f) || !near(wrap(b[2] - a[2]), 0, 0.02f)) bad++;
        }
        CHECK(bad == 0, "basis_to_angles disagrees with AngleVectors for %d of 2000 random angles", bad);
    }

    /* 2. the axis map is a rotation: OpenXR identity looks along Source +X, left of the head is Source +Y, up is +Z */
    {
        float f[3], l[3], u[3];
        bmvr_eye_basis(ident, f, l, u);
        CHECK(near(f[0], 1, 1e-5f) && near(f[1], 0, 1e-5f) && near(f[2], 0, 1e-5f), "identity forward = (%g %g %g), want +X", f[0], f[1], f[2]);
        CHECK(near(l[1], 1, 1e-5f) && near(l[0], 0, 1e-5f), "identity left = (%g %g %g), want +Y", l[0], l[1], l[2]);
        CHECK(near(u[2], 1, 1e-5f), "identity up = (%g %g %g), want +Z", u[0], u[1], u[2]);
    }

    /* 3. at the recenter, looking ahead, game camera at (100, 200, 50) yaw 0: eyes sit 32 mm left/right of it */
    {
        struct bmvr_stereo_rig rig; bmvr_stereo_rig_init(&rig, S);
        struct bmvr_xfer_pose p = make_pose(ident, head0);
        const float go[3] = { 100, 200, 50 };
        struct bmvr_view v[2];
        CHECK(bmvr_stereo_eyes(&rig, &p, go, 0.0f, v), "eyes() failed on a good pose");
        CHECK(near(v[0].origin[0], 100, 0.01f) && near(v[0].origin[1], 200 + 0.032f * S, 0.01f) && near(v[0].origin[2], 50, 0.01f),
              "left eye at (%g %g %g), want (100, %g, 50): the left eye is +Y", v[0].origin[0], v[0].origin[1], v[0].origin[2], 200 + 0.032f * S);
        CHECK(near(v[1].origin[1], 200 - 0.032f * S, 0.01f), "right eye y = %g, want %g", v[1].origin[1], 200 - 0.032f * S);
        CHECK(near(v[0].angles[0], 0, 0.01f) && near(v[0].angles[1], 0, 0.01f) && near(v[0].angles[2], 0, 0.01f),
              "angles (%g %g %g), want 0 0 0", v[0].angles[0], v[0].angles[1], v[0].angles[2]);
    }

    /* 4. the game turns left 90 degrees (yaw 90): the eye offset turns with it; the left eye of a player facing +Y is on -X */
    {
        struct bmvr_stereo_rig rig; bmvr_stereo_rig_init(&rig, S);
        struct bmvr_xfer_pose p = make_pose(ident, head0);
        const float go[3] = { 0, 0, 0 };
        struct bmvr_view v[2];
        bmvr_stereo_recenter(&rig, &p);
        CHECK(bmvr_stereo_eyes(&rig, &p, go, 90.0f, v), "eyes() failed");
        CHECK(near(v[0].origin[0], -0.032f * S, 0.01f) && near(v[0].origin[1], 0, 0.01f), "left eye at (%g %g), want (%g 0)", v[0].origin[0], v[0].origin[1], -0.032f * S);
        CHECK(near(v[0].angles[1], 90, 0.01f), "yaw %g, want 90", v[0].angles[1]);
    }

    /* 5. head turns 30 degrees to the LEFT after the recenter (OpenXR: +30 about +Y): Source yaw +30, pitch/roll 0 */
    {
        struct bmvr_stereo_rig rig; bmvr_stereo_rig_init(&rig, S);
        struct bmvr_xfer_pose p0 = make_pose(ident, head0);
        bmvr_stereo_recenter(&rig, &p0);
        float q[4]; quat_axis(30, 0, 1, 0, q);
        struct bmvr_xfer_pose p = make_pose(q, head0);
        const float go[3] = { 0, 0, 0 };
        struct bmvr_view v[2];
        CHECK(bmvr_stereo_eyes(&rig, &p, go, 0.0f, v), "eyes() failed");
        CHECK(near(v[1].angles[1], 30, 0.02f) && near(v[1].angles[0], 0, 0.02f) && near(v[1].angles[2], 0, 0.02f), "turn-left angles (%g %g %g), want (0 30 0)", v[1].angles[0], v[1].angles[1], v[1].angles[2]);
    }

    /* 6. head looks UP 20 degrees (+20 about +X): Source pitch is NEGATIVE (positive pitch looks down) */
    {
        struct bmvr_stereo_rig rig; bmvr_stereo_rig_init(&rig, S);
        struct bmvr_xfer_pose p0 = make_pose(ident, head0);
        bmvr_stereo_recenter(&rig, &p0);
        float q[4]; quat_axis(20, 1, 0, 0, q);
        struct bmvr_xfer_pose p = make_pose(q, head0);
        const float go[3] = { 0, 0, 0 };
        struct bmvr_view v[2];
        CHECK(bmvr_stereo_eyes(&rig, &p, go, 0.0f, v), "eyes() failed");
        CHECK(near(v[0].angles[0], -20, 0.02f) && near(v[0].angles[1], 0, 0.02f) && near(v[0].angles[2], 0, 0.02f), "look-up angles (%g %g %g), want (-20 0 0)", v[0].angles[0], v[0].angles[1], v[0].angles[2]);
    }

    /* 7. a random orientation: the engine's AngleVectors() of our angles gives the same forward/right/up as the geometric map */
    {
        int bad = 0;
        srand(11);
        for (int i = 0; i < 500; i++) {
            float qa[4], qb[4], qc[4], q[4];
            quat_axis((float)(rand() % 360), 0, 1, 0, qa);
            quat_axis((float)(rand() % 120 - 60), 1, 0, 0, qb);
            quat_axis((float)(rand() % 80 - 40), 0, 0, 1, qc);
            float t[4]; quat_mul(qa, qb, t); quat_mul(t, qc, q);
            struct bmvr_stereo_rig rig; bmvr_stereo_rig_init(&rig, S);
            /* reference = looking straight ahead: yaw turn 0 */
            struct bmvr_xfer_pose pr = make_pose(ident, head0);
            bmvr_stereo_recenter(&rig, &pr);
            struct bmvr_xfer_pose p = make_pose(q, head0);
            const float go[3] = { 0, 0, 0 };
            struct bmvr_view v[2];
            if (!bmvr_stereo_eyes(&rig, &p, go, 0.0f, v)) { bad++; continue; }
            /* the rig yaw turn is the game yaw (0) minus the REFERENCE yaw (0 for the identity reference): no extra turn */
            float f[3], r[3], u[3], F[3], L[3], U[3];
            bmvr_angle_vectors(v[0].angles, f, r, u);
            bmvr_eye_basis(q, F, L, U);
            for (int k = 0; k < 3; k++) if (!near(f[k], F[k], 2e-3f) || !near(-r[k], L[k], 2e-3f) || !near(u[k], U[k], 2e-3f)) { bad++; break; }
        }
        CHECK(bad == 0, "angles disagree with the geometric basis for %d of 500 random head orientations", bad);
    }

    /* 8. room-scale motion: 0.5 m forward and 0.1 m up after the recenter -> origin moves +X 0.5 m, +Z 0.1 m (game yaw 0) */
    {
        struct bmvr_stereo_rig rig; bmvr_stereo_rig_init(&rig, S);
        struct bmvr_xfer_pose p0 = make_pose(ident, head0);
        bmvr_stereo_recenter(&rig, &p0);
        const float head1[3] = { head0[0], head0[1] + 0.1f, head0[2] - 0.5f };
        struct bmvr_xfer_pose p = make_pose(ident, head1);
        const float go[3] = { 10, 20, 30 };
        struct bmvr_view v[2];
        CHECK(bmvr_stereo_eyes(&rig, &p, go, 0.0f, v), "eyes() failed");
        CHECK(near(v[0].origin[0], 10 + 0.5f * S, 0.01f) && near(v[0].origin[2], 30 + 0.1f * S, 0.01f), "moved eye at (%g %g %g)", v[0].origin[0], v[0].origin[1], v[0].origin[2]);
        /* with the game turned 90 degrees left the same step goes along +Y, and the left eye's 32 mm sideways offset (+Y)
         * turns into -X: (0.5 m fwd, 32 mm left) -> (-32 mm, +0.5 m) */
        CHECK(bmvr_stereo_eyes(&rig, &p, go, 90.0f, v), "eyes() failed");
        CHECK(near(v[0].origin[0], 10 - 0.032f * S, 0.01f) && near(v[0].origin[1], 20 + 0.5f * S, 0.01f),
              "stepped with the game turned: (%g %g), want (%g %g)", v[0].origin[0], v[0].origin[1], 10 - 0.032f * S, 20 + 0.5f * S);
    }

    /* 9. the frustum: rebuild what the engine makes from fov + aspect + fractions and compare with the tangents we started from */
    {
        const float fovs[][4] = { { -0.8f, 0.8f, 0.8f, -0.8f }, { -0.95f, 0.65f, 0.78f, -0.88f }, { -0.5f, 1.0f, 0.6f, -0.9f }, { -1.0f, 0.7f, 0.9f, -0.5f } };
        for (unsigned k = 0; k < sizeof fovs / sizeof fovs[0]; k++) {
            float fd = 0, as = 1, fr[4] = { 0, 0, 0, 0 };
            CHECK(bmvr_eye_frustum(fovs[k], &fd, &as, fr), "frustum %u rejected", k);
            const float hw = tanf(fd * BMVR_PI_F / 360.0f), hh = hw / as;
            const float L = -hw + 2 * hw * fr[0], R = -hw + 2 * hw * fr[1], B = -hh + 2 * hh * fr[2], T = -hh + 2 * hh * fr[3];
            CHECK(near(L, tanf(fovs[k][0]), 1e-4f) && near(R, tanf(fovs[k][1]), 1e-4f) && near(T, tanf(fovs[k][2]), 1e-4f) && near(B, tanf(fovs[k][3]), 1e-4f),
                  "frustum %u rebuilt as l=%g r=%g u=%g d=%g, want %g %g %g %g", k, L, R, T, B, tanf(fovs[k][0]), tanf(fovs[k][1]), tanf(fovs[k][2]), tanf(fovs[k][3]));
            CHECK(fr[0] >= 0 && fr[1] <= 1.0001f && fr[2] >= 0 && fr[3] <= 1.0001f && fr[0] < fr[1] && fr[2] < fr[3], "fractions out of range for %u", k);
        }
        float fd = 0, as = 1, fr[4] = { 0, 0, 0, 0 };
        const float sym[4] = { -0.8f, 0.8f, 0.8f, -0.8f };
        bmvr_eye_frustum(sym, &fd, &as, fr);
        CHECK(near(fr[0], 0, 1e-5f) && near(fr[1], 1, 1e-5f) && near(fr[2], 0, 1e-5f) && near(fr[3], 1, 1e-5f) && near(as, 1, 1e-5f) && near(fd, 2 * 0.8f * 180 / BMVR_PI_F * 1.0f, 3.0f),
              "symmetric field: fractions %g %g %g %g aspect %g fov %g", fr[0], fr[1], fr[2], fr[3], as, fd);
        /* tangent-of-half-angle: a 2*atan(tan(0.8)) fov */
        CHECK(near(fd, 2.0f * 0.8f * 180.0f / BMVR_PI_F, 1e-3f), "fov %g, want %g", fd, 2.0f * 0.8f * 180.0f / BMVR_PI_F);
    }

    /* 10. unusable input is refused, never turned into NaN views */
    {
        struct bmvr_stereo_rig rig; bmvr_stereo_rig_init(&rig, S);
        struct bmvr_xfer_pose p = make_pose(ident, head0);
        const float go[3] = { 0, 0, 0 };
        struct bmvr_view v[2];
        struct bmvr_xfer_pose bad = p; bad.eye[1].rot[3] = 0; bad.eye[1].rot[0] = bad.eye[1].rot[1] = bad.eye[1].rot[2] = 0;
        CHECK(!bmvr_stereo_eyes(&rig, &bad, go, 0, v), "a zero quaternion was accepted");
        bad = p; bad.eye[0].fov[1] = bad.eye[0].fov[0];
        { struct bmvr_stereo_rig r2; bmvr_stereo_rig_init(&r2, S); CHECK(!bmvr_stereo_eyes(&r2, &bad, go, 0, v), "an empty field of view was accepted"); }
        bad = p; bad.eye[0].rot[0] = NAN;
        { struct bmvr_stereo_rig r2; bmvr_stereo_rig_init(&r2, S); CHECK(!bmvr_stereo_eyes(&r2, &bad, go, 0, v), "a NaN quaternion was accepted"); }
        bad = p; bad.eye[0].fov[2] = 1.57f; bad.eye[0].fov[3] = -1.57f;   /* ~ +-1000 tangent: absurd */
        { struct bmvr_stereo_rig r2; bmvr_stereo_rig_init(&r2, S); CHECK(!bmvr_stereo_eyes(&r2, &bad, go, 0, v), "a 180-degree field of view was accepted"); }
        /* a slightly non-unit quaternion (rounding in the runtime) is normalised, not refused */
        bad = p; for (int e = 0; e < 2; e++) { bad.eye[e].rot[3] = 1.02f; }
        { struct bmvr_stereo_rig r2; bmvr_stereo_rig_init(&r2, S); CHECK(bmvr_stereo_eyes(&r2, &bad, go, 0, v), "a near-unit quaternion was refused"); }
    }

    printf("stereo_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
