/*
 * Black Mesa VR - native Linux port, slice 3a: controller state -> game actions.
 *
 * Pure logic, no engine, no libc beyond <math.h>/<string.h> (same style as bmvr_stereo.h, so tests/linux/input_map_test.c checks every
 * rule without a game or a headset). Input is what the helper delivers in every ack (struct bmvr_xfer_input, v6, read with
 * bmvr_get_input()); output is a neutral command the CreateMove hook (slice 3b) copies into the engine's CUserCmd.
 *
 * Steam Frame layout (verified by the slice-2 log: every control below arrived on a headset run, except X/Y and the left d-pad, which
 * were not pressed in that run):
 *   left hand : thumbstick, trigger, squeeze, bumper, d-pad up/down (BTN_SECONDARY / BTN_PRIMARY), view (BTN_MENU), stick click
 *   right hand: thumbstick, trigger, squeeze, bumper, A (PRIMARY), B (SECONDARY), X (EXTRA1), Y (EXTRA2), menu (MENU), stick click
 * The Windows build's README scheme (left X / left Y) cannot be copied literally: the Frame's left hand has no X/Y.
 *
 *   control                   game action
 *   left stick                walk (forward / side)
 *   right stick left/right    turn (the hook applies snap or smooth turning)
 *   right stick up / down     jump / crouch (same rules as the Windows build: vertical enough, and not while the stick was just used to turn)
 *   right trigger             primary attack (IN_ATTACK)
 *   left trigger              use (IN_USE)
 *   right B                   alt fire (IN_ATTACK2)
 *   right A                   reload (IN_RELOAD)
 *   right bumper              jump (IN_JUMP)
 *   right X                   crouch, held (IN_DUCK)
 *   left stick click          sprint, held (IN_SPEED)
 *   right stick click         tap: recenter; hold >= wheel_hold_ms: weapon wheel (open on hold, select on release)
 *   right squeeze             flashlight (edge: impulse 100)
 *   left d-pad down / up      previous / next weapon (edges)
 *   left view or right menu   pause (edge)
 *   unmapped                  left bumper, right Y, left squeeze (reserved for two-hand grip, handled by the hand code)
 *
 * While the menu owns the controls (menu_open): right trigger or A = select, B = back, d-pad = move; nothing reaches the game.
 */
#ifndef BMVR_INPUT_MAP_H
#define BMVR_INPUT_MAP_H

#include <math.h>
#include <string.h>
#include "bmvr_xfer.h"
#include "bmvr_stereo.h"

/* Same numeric values as the Source engine's IN_* bits (src/sdk/in_buttons.h), so the hook can OR them straight into CUserCmd::buttons. */
#define BMVR_IN_ATTACK   (1u << 0)
#define BMVR_IN_JUMP     (1u << 1)
#define BMVR_IN_DUCK     (1u << 2)
#define BMVR_IN_USE      (1u << 5)
#define BMVR_IN_ATTACK2  (1u << 11)
#define BMVR_IN_RELOAD   (1u << 13)
#define BMVR_IN_SPEED    (1u << 17)

/* One-shot events: set for exactly one step() when the control goes down (or, for WHEEL_SELECT, comes up). */
#define BMVR_EV_FLASHLIGHT    1u
#define BMVR_EV_WEAPON_PREV   2u
#define BMVR_EV_WEAPON_NEXT   4u
#define BMVR_EV_PAUSE         8u
#define BMVR_EV_RECENTER      16u
#define BMVR_EV_WHEEL_OPEN    32u
#define BMVR_EV_WHEEL_SELECT  64u   /* the stick click that opened the wheel was released: the hook picks the hovered icon */
#define BMVR_EV_MENU_SELECT   128u
#define BMVR_EV_MENU_BACK     256u
#define BMVR_EV_MENU_UP       512u
#define BMVR_EV_MENU_DOWN     1024u

/* What a face button can do (input_btn_*= in bmvr_target.txt). */
#define BMVR_ACT_NONE    0
#define BMVR_ACT_RELOAD  1
#define BMVR_ACT_JUMP    2
#define BMVR_ACT_DUCK    3
#define BMVR_ACT_ATTACK2 4
#define BMVR_ACT_USE     5
#define BMVR_ACT_ATTACK  6
static inline unsigned bmvr_im_act_bits(int act)
{
    switch (act) {
    case BMVR_ACT_RELOAD:  return BMVR_IN_RELOAD;
    case BMVR_ACT_JUMP:    return BMVR_IN_JUMP;
    case BMVR_ACT_DUCK:    return BMVR_IN_DUCK;
    case BMVR_ACT_ATTACK2: return BMVR_IN_ATTACK2;
    case BMVR_ACT_USE:     return BMVR_IN_USE;
    case BMVR_ACT_ATTACK:  return BMVR_IN_ATTACK;
    default: return 0;
    }
}

struct bmvr_cmd {
    float forward, side;        /* -1..1, +forward, +right; dead zone applied and rescaled */
    float turn;                 /* -1..1, +right; 0 while the wheel is open or the menu owns the stick */
    unsigned buttons;           /* BMVR_IN_*: held this step */
    unsigned events;            /* BMVR_EV_*: one step only */
    int wheel_open;             /* the weapon wheel is up (analog turn/jump/crouch are suppressed meanwhile) */
};

struct bmvr_input_map_cfg {
    float walk_dead;            /* default 0.15 */
    float trigger_on, trigger_off;   /* hysteresis, default 0.55 / 0.35 */
    float stick_jump_crouch;    /* how far up/down the right stick must go, default 0.75 (the Windows StickJumpCrouchThreshold) */
    float wheel_hold_ms;        /* default 180, the Windows value */
    int   turn_blocks_jump_crouch;   /* default 1 */
    int   menu_open;            /* set by the caller every step */
    int   act_a, act_b, act_bumper, act_x, act_y;   /* BMVR_ACT_*: the right hand's face buttons and bumper (after the hand swap) */
};

struct bmvr_input_map {
    struct bmvr_input_map_cfg cfg;
    unsigned prev_btn;          /* previous BMVR_XFER buttons, both hands (hand 1 shifted by 8) */
    int atk_on, use_on, flash_on;       /* analog -> digital with hysteresis */
    int prev_atk;               /* the right trigger's previous digital state (menu select edge) */
    int stick_turned, crouch_held;      /* right-stick jump/crouch bookkeeping, as in the Windows build */
    float click_ms;             /* how long the right stick click has been down */
    int wheel_open;
    int crouch_toggled;
    int primed;                 /* the first step only records the previous state: a button held at start is not an edge */
};

static inline void bmvr_input_map_init(struct bmvr_input_map *m)
{
    memset(m, 0, sizeof *m);
    m->cfg.walk_dead = 0.15f;
    m->cfg.trigger_on = 0.55f;
    m->cfg.trigger_off = 0.35f;
    m->cfg.stick_jump_crouch = 0.75f;
    m->cfg.wheel_hold_ms = 180.0f;
    m->cfg.turn_blocks_jump_crouch = 1;
    m->cfg.act_a = BMVR_ACT_RELOAD;
    m->cfg.act_b = BMVR_ACT_ATTACK2;
    m->cfg.act_bumper = BMVR_ACT_JUMP;
    m->cfg.act_x = BMVR_ACT_DUCK;
    m->cfg.act_y = BMVR_ACT_NONE;
}

/* Hand assignment: swap_hands mirrors everything (left-handed: weapon in the left hand, walk on the right stick); swap_sticks only exchanges
 * the two sticks (so one hand can both walk and fire). */
/* The direction a controller points, as Source (pitch, yaw), given its aim pose (pos xyz, quat xyzw, OpenXR LOCAL space) and how far the game's
 * body yaw is from the stereo reference (the same `turn` the eye views use, so the aim and the picture agree). 0 for an unusable quaternion. */
static inline int bmvr_im_aim_angles(const float aim[7], float turn_deg, float out[2])
{
    float q[4], f[3], t[3], r[3];
    if (!bmvr_quat_ok(aim + 3, q)) return 0;
    const float fx[3] = { 0.0f, 0.0f, -1.0f };
    bmvr_quat_rotate(q, fx, t);
    bmvr_xr_to_src(t, f);
    bmvr_rot_z(f, turn_deg, r);
    const float xy = sqrtf(r[0] * r[0] + r[1] * r[1]);
    out[0] = atan2f(-r[2], xy) * 180.0f / BMVR_PI_F;
    out[1] = xy > 0.001f ? atan2f(r[1], r[0]) * 180.0f / BMVR_PI_F : 0.0f;
    if (out[0] > 89.0f) out[0] = 89.0f;
    if (out[0] < -89.0f) out[0] = -89.0f;
    return 1;
}

static inline void bmvr_im_remap_hands(struct bmvr_xfer_input *in, int swap_hands, int swap_sticks)
{
    if (swap_hands) {
        float t;
        { const unsigned p = in->flags & (BMVR_XFER_INPUT_POSE_L | BMVR_XFER_INPUT_POSE_R);
          in->flags = (in->flags & ~(BMVR_XFER_INPUT_POSE_L | BMVR_XFER_INPUT_POSE_R)) | ((p & BMVR_XFER_INPUT_POSE_L) ? BMVR_XFER_INPUT_POSE_R : 0u) | ((p & BMVR_XFER_INPUT_POSE_R) ? BMVR_XFER_INPUT_POSE_L : 0u); }
        in->buttons = ((in->buttons & 0xffu) << 8) | ((in->buttons >> 8) & 0xffu);
        for (int i = 0; i < 2; i++) { t = in->stick[0][i]; in->stick[0][i] = in->stick[1][i]; in->stick[1][i] = t; }
        t = in->trigger[0]; in->trigger[0] = in->trigger[1]; in->trigger[1] = t;
        t = in->squeeze[0]; in->squeeze[0] = in->squeeze[1]; in->squeeze[1] = t;
        for (int i = 0; i < 7; i++) {
            t = in->aim[0][i]; in->aim[0][i] = in->aim[1][i]; in->aim[1][i] = t;
            t = in->grip[0][i]; in->grip[0][i] = in->grip[1][i]; in->grip[1][i] = t;
        }
    }
    if (swap_sticks) for (int i = 0; i < 2; i++) { const float t = in->stick[0][i]; in->stick[0][i] = in->stick[1][i]; in->stick[1][i] = t; }
}

static inline float bmvr_im_dead(float v, float dz)
{
    const float a = fabsf(v);
    if (a <= dz) return 0.0f;
    const float t = (a - dz) / (1.0f - dz);
    return v < 0.0f ? -t : t;
}

static inline int bmvr_im_hyst(int on, float v, float hi, float lo)
{
    if (on) return v > lo;
    return v >= hi;
}

/* Walk stick -> forwardmove / sidemove. The stick is relative to where the HEAD looks; the cmd is relative to the game's yaw.
 * head_delta_deg = how far the head yaw is from the game yaw, in Source degrees (positive = head turned LEFT, same sign as
 * CUserCmd::viewangles[1]). Rotates the (forward, side) stick vector into the game's frame and scales it by max_speed
 * (Source cl_forwardspeed, 450). Pure: no state, NULL outputs are skipped. */
static inline void bmvr_im_move_relative(float forward, float side, float head_delta_deg, float max_speed, float *fwd_out, float *side_out)
{
    const float r = head_delta_deg * (3.14159265358979f / 180.0f);
    const float cs = cosf(r), sn = sinf(r);
    float f = forward * cs + side * sn;       /* along the game's forward */
    float s = side * cs - forward * sn;       /* along the game's right */
    const float len = sqrtf(f * f + s * s);
    if (len > 1.0f) { f /= len; s /= len; }
    if (fwd_out) *fwd_out = f * max_speed;
    if (side_out) *side_out = s * max_speed;
}

/* One step per game tick. dt_ms: time since the previous step. `in` may be NULL or unfocused: everything is released (and an open wheel
 * is cancelled, not selected). */
static inline void bmvr_input_map_step(struct bmvr_input_map *m, const struct bmvr_xfer_input *in, float dt_ms, struct bmvr_cmd *out)
{
    memset(out, 0, sizeof *out);
    struct bmvr_xfer_input z;
    if (!in || !in->seq || !(in->flags & BMVR_XFER_INPUT_FOCUSED)) {
        memset(&z, 0, sizeof z);
        in = &z;
        m->wheel_open = 0;
        m->click_ms = 0.0f;
    }
    const unsigned b = in->buttons;
    const unsigned pressed = b & ~m->prev_btn;
    const unsigned L = 0, R = 8;
#define DOWN(h, bit)    (((b) >> (h)) & (bit))
#define PRESSED(h, bit) (((pressed) >> (h)) & (bit))
    const int first = !m->primed;
    m->primed = 1;
#undef PRESSED
#define PRESSED(h, bit) (first ? 0u : (((pressed) >> (h)) & (bit)))

    m->atk_on = bmvr_im_hyst(m->atk_on, in->trigger[1], m->cfg.trigger_on, m->cfg.trigger_off);
    m->use_on = bmvr_im_hyst(m->use_on, in->trigger[0], m->cfg.trigger_on, m->cfg.trigger_off);
    const int flash_was = m->flash_on;
    m->flash_on = bmvr_im_hyst(m->flash_on, in->squeeze[1], 0.8f, 0.5f);

    /* menu: the controls belong to the menu, nothing reaches the game */
    if (m->cfg.menu_open) {
        if (m->atk_on && !m->prev_atk && !first) out->events |= BMVR_EV_MENU_SELECT;
        if (PRESSED(R, BMVR_XFER_BTN_PRIMARY)) out->events |= BMVR_EV_MENU_SELECT;
        if (PRESSED(R, BMVR_XFER_BTN_SECONDARY)) out->events |= BMVR_EV_MENU_BACK;
        if (PRESSED(L, BMVR_XFER_BTN_SECONDARY)) out->events |= BMVR_EV_MENU_UP;
        if (PRESSED(L, BMVR_XFER_BTN_PRIMARY)) out->events |= BMVR_EV_MENU_DOWN;
        if (PRESSED(L, BMVR_XFER_BTN_MENU) || PRESSED(R, BMVR_XFER_BTN_MENU)) out->events |= BMVR_EV_PAUSE;
        m->wheel_open = 0;
        m->click_ms = 0.0f;
        m->prev_btn = b;
        m->prev_atk = m->atk_on;
        return;
    }

    /* weapon wheel on the right stick click: tap recenters, hold opens, release selects */
    if (DOWN(R, BMVR_XFER_BTN_STICK)) {
        m->click_ms += dt_ms;
        if (!m->wheel_open && m->click_ms >= m->cfg.wheel_hold_ms) {
            m->wheel_open = 1;
            out->events |= BMVR_EV_WHEEL_OPEN;
        }
    } else {
        if (m->click_ms > 0.0f && !first) {
            if (m->wheel_open) out->events |= BMVR_EV_WHEEL_SELECT;
            else out->events |= BMVR_EV_RECENTER;
        }
        m->click_ms = 0.0f;
        m->wheel_open = 0;
    }
    out->wheel_open = m->wheel_open;

    /* movement */
    out->forward = bmvr_im_dead(in->stick[0][1], m->cfg.walk_dead);
    out->side = bmvr_im_dead(in->stick[0][0], m->cfg.walk_dead);

    /* right stick: turn, and jump / crouch from straight up / down */
    float tx = in->stick[1][0], ty = in->stick[1][1];
    int stick_jump = 0, stick_crouch = 0;
    if (!m->wheel_open) {
        const float ax = fabsf(tx), ay = fabsf(ty);
        const int centred = ax * ax + ay * ay < 0.25f * 0.25f;
        if (centred) m->stick_turned = 0;
        else if (ax > 0.5f && ax > ay && !m->crouch_held) m->stick_turned = 1;
        const int vertical = ay >= m->cfg.stick_jump_crouch && ax <= ay * 0.577f;
        if (vertical && !(m->cfg.turn_blocks_jump_crouch && m->stick_turned)) {
            stick_jump = ty > 0.0f;
            stick_crouch = ty < 0.0f;
        }
        if (stick_crouch) m->crouch_held = 1;
        else if (centred || ty >= 0.25f) m->crouch_held = 0;
        if (m->crouch_held) stick_crouch = 1;
        out->turn = bmvr_im_dead(tx, 0.25f);
    } else {
        m->crouch_held = 0;
    }

    unsigned btn = 0;
    if (m->atk_on) btn |= BMVR_IN_ATTACK;
    if (m->use_on) btn |= BMVR_IN_USE;
    if (DOWN(R, BMVR_XFER_BTN_SECONDARY)) btn |= bmvr_im_act_bits(m->cfg.act_b);
    if (DOWN(R, BMVR_XFER_BTN_PRIMARY)) btn |= bmvr_im_act_bits(m->cfg.act_a);
    if (DOWN(R, BMVR_XFER_BTN_BUMPER)) btn |= bmvr_im_act_bits(m->cfg.act_bumper);
    if (DOWN(R, BMVR_XFER_BTN_EXTRA1)) btn |= bmvr_im_act_bits(m->cfg.act_x);
    if (DOWN(R, BMVR_XFER_BTN_EXTRA2)) btn |= bmvr_im_act_bits(m->cfg.act_y);
    if (stick_jump) btn |= BMVR_IN_JUMP;
    if (stick_crouch) btn |= BMVR_IN_DUCK;
    if (DOWN(L, BMVR_XFER_BTN_STICK)) btn |= BMVR_IN_SPEED;
    out->buttons = btn;

    if (m->flash_on && !flash_was && !first) out->events |= BMVR_EV_FLASHLIGHT;
    if (PRESSED(L, BMVR_XFER_BTN_PRIMARY)) out->events |= BMVR_EV_WEAPON_PREV;
    if (PRESSED(L, BMVR_XFER_BTN_SECONDARY)) out->events |= BMVR_EV_WEAPON_NEXT;
    if (PRESSED(L, BMVR_XFER_BTN_MENU) || PRESSED(R, BMVR_XFER_BTN_MENU)) out->events |= BMVR_EV_PAUSE;

    m->prev_btn = b;
    m->prev_atk = m->atk_on;
#undef DOWN
#undef PRESSED
}

#endif