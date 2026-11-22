/* Slice 3a: the controller -> game action mapping (linux/bmvr_input_map.h). No GPU, no engine, no headset.
 *   cc -O2 -Wall -Wextra -std=gnu11 -I linux -o input_map_test tests/linux/input_map_test.c -lm && ./input_map_test */
#include <stdio.h>
#include "bmvr_input_map.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static struct bmvr_xfer_input mk(void) { struct bmvr_xfer_input i; memset(&i, 0, sizeof i); i.seq = 1; i.flags = BMVR_XFER_INPUT_FOCUSED; return i; }
static struct bmvr_cmd step(struct bmvr_input_map *m, struct bmvr_xfer_input *i, float dt) { struct bmvr_cmd c; i->seq++; bmvr_input_map_step(m, i, dt, &c); return c; }
#define LB(x) ((x) << 0)
#define RB(x) ((x) << 8)

int main(void)
{
    struct bmvr_input_map m; bmvr_input_map_init(&m);
    struct bmvr_xfer_input i = mk();
    struct bmvr_cmd c = step(&m, &i, 11.0f);

    /* walking: dead zone then rescale, forward is +y */
    i.stick[0][1] = 1.0f; i.stick[0][0] = -1.0f; c = step(&m, &i, 11.0f);
    CHECK(near(c.forward, 1.0f) && near(c.side, -1.0f), "full stick gives full walk (%.3f %.3f)", c.forward, c.side);
    i.stick[0][1] = 0.10f; i.stick[0][0] = 0.14f; c = step(&m, &i, 11.0f);
    CHECK(c.forward == 0.0f && c.side == 0.0f, "inside the dead zone is nothing");
    i.stick[0][1] = 0.575f; i.stick[0][0] = 0.0f; c = step(&m, &i, 11.0f);
    CHECK(near(c.forward, 0.5f), "rescaled past the dead zone (%.3f)", c.forward);
    memset(&i.stick, 0, sizeof i.stick);

    /* right trigger = attack with hysteresis; left trigger = use */
    i.trigger[1] = 0.60f; c = step(&m, &i, 11.0f);
    CHECK(c.buttons & BMVR_IN_ATTACK, "right trigger 0.60 attacks");
    i.trigger[1] = 0.40f; c = step(&m, &i, 11.0f);
    CHECK(c.buttons & BMVR_IN_ATTACK, "hysteresis: still held at 0.40");
    i.trigger[1] = 0.30f; c = step(&m, &i, 11.0f);
    CHECK(!(c.buttons & BMVR_IN_ATTACK), "released below 0.35");
    i.trigger[1] = 0.0f; i.trigger[0] = 0.94f; c = step(&m, &i, 11.0f);
    CHECK((c.buttons & BMVR_IN_USE) && !(c.buttons & BMVR_IN_ATTACK), "left trigger uses, does not attack");
    i.trigger[0] = 0.0f; c = step(&m, &i, 11.0f);

    /* the log's own bit patterns: R 0x1 = A reload, R 0x2 = B alt fire, L 0x8 = sprint, R 0x10 = bumper jump */
    i.buttons = RB(BMVR_XFER_BTN_PRIMARY);   c = step(&m, &i, 11.0f); CHECK(c.buttons == BMVR_IN_RELOAD, "A reloads");
    i.buttons = RB(BMVR_XFER_BTN_SECONDARY); c = step(&m, &i, 11.0f); CHECK(c.buttons == BMVR_IN_ATTACK2, "B alt-fires");
    i.buttons = LB(BMVR_XFER_BTN_STICK);     c = step(&m, &i, 11.0f); CHECK(c.buttons == BMVR_IN_SPEED, "left stick click sprints");
    i.buttons = RB(BMVR_XFER_BTN_BUMPER);    c = step(&m, &i, 11.0f); CHECK(c.buttons == BMVR_IN_JUMP, "right bumper jumps");
    i.buttons = RB(BMVR_XFER_BTN_EXTRA1);    c = step(&m, &i, 11.0f); CHECK(c.buttons == BMVR_IN_DUCK, "right X crouches while held");
    i.buttons = 0; c = step(&m, &i, 11.0f); CHECK(c.buttons == 0 && c.events == 0, "all released is quiet");

    /* edges fire once */
    i.buttons = LB(BMVR_XFER_BTN_PRIMARY); c = step(&m, &i, 11.0f); CHECK(c.events == BMVR_EV_WEAPON_PREV, "d-pad down: previous weapon");
    c = step(&m, &i, 11.0f); CHECK(c.events == 0, "...once, not every step");
    i.buttons = LB(BMVR_XFER_BTN_SECONDARY); c = step(&m, &i, 11.0f); CHECK(c.events == BMVR_EV_WEAPON_NEXT, "d-pad up: next weapon");
    i.buttons = LB(BMVR_XFER_BTN_MENU); c = step(&m, &i, 11.0f); CHECK(c.events == BMVR_EV_PAUSE, "left view pauses");
    i.buttons = RB(BMVR_XFER_BTN_MENU); c = step(&m, &i, 11.0f); CHECK(c.events == BMVR_EV_PAUSE, "right menu pauses");
    i.buttons = 0; c = step(&m, &i, 11.0f);

    /* flashlight: right squeeze, once per squeeze */
    i.squeeze[1] = 1.0f; c = step(&m, &i, 11.0f); CHECK(c.events == BMVR_EV_FLASHLIGHT, "squeeze toggles the flashlight");
    c = step(&m, &i, 11.0f); CHECK(c.events == 0, "held squeeze does not retoggle");
    i.squeeze[1] = 0.6f; c = step(&m, &i, 11.0f); i.squeeze[1] = 0.0f; c = step(&m, &i, 11.0f);
    i.squeeze[1] = 0.59f; c = step(&m, &i, 11.0f); CHECK(c.events == 0, "a light grip (0.59) is not a flashlight press");
    i.squeeze[1] = 0.0f; c = step(&m, &i, 11.0f);

    /* right stick click: tap recenters, hold opens the wheel and release selects */
    i.buttons = RB(BMVR_XFER_BTN_STICK); c = step(&m, &i, 11.0f);
    i.buttons = 0; c = step(&m, &i, 11.0f);
    CHECK(c.events == BMVR_EV_RECENTER, "short tap recenters");
    i.buttons = RB(BMVR_XFER_BTN_STICK);
    unsigned ev = 0; int open = 0;
    for (int k = 0; k < 20; k++) { c = step(&m, &i, 11.0f); ev |= c.events; open = c.wheel_open; }
    CHECK((ev & BMVR_EV_WHEEL_OPEN) && open, "held past 180 ms opens the wheel");
    CHECK(!(ev & BMVR_EV_RECENTER), "a hold never recenters");
    i.stick[1][0] = 1.0f; i.stick[1][1] = -1.0f; c = step(&m, &i, 11.0f);
    CHECK(c.turn == 0.0f && !(c.buttons & BMVR_IN_DUCK), "right stick is the wheel's, not turn/crouch, while it is open");
    memset(&i.stick, 0, sizeof i.stick);
    i.buttons = 0; c = step(&m, &i, 11.0f);
    CHECK(c.events == BMVR_EV_WHEEL_SELECT && !c.wheel_open, "release on the wheel selects");

    /* losing focus cancels an open wheel (no select) */
    i.buttons = RB(BMVR_XFER_BTN_STICK);
    for (int k = 0; k < 20; k++) c = step(&m, &i, 11.0f);
    i.flags = 0; c = step(&m, &i, 11.0f);
    CHECK(!c.wheel_open && c.events == 0, "focus loss cancels the wheel quietly");
    i.flags = BMVR_XFER_INPUT_FOCUSED; i.buttons = 0; c = step(&m, &i, 11.0f);
    CHECK(c.events == 0, "and releasing afterwards selects nothing");

    /* right stick: turn, jump, crouch, and turning does not jump/crouch */
    memset(&i, 0, sizeof i); i.seq = 100; i.flags = BMVR_XFER_INPUT_FOCUSED; m.stick_turned = m.crouch_held = 0;
    i.stick[1][0] = 0.9f; c = step(&m, &i, 11.0f);
    CHECK(c.turn > 0.8f && !(c.buttons & (BMVR_IN_JUMP | BMVR_IN_DUCK)), "stick right turns right, nothing else");
    memset(&i.stick, 0, sizeof i.stick); c = step(&m, &i, 11.0f);   /* back to centre: the turn is over */
    i.stick[1][1] = 0.95f; c = step(&m, &i, 11.0f);
    CHECK((c.buttons & BMVR_IN_JUMP) && c.turn == 0.0f, "stick up jumps");
    i.stick[1][1] = -0.95f; c = step(&m, &i, 11.0f);
    CHECK(c.buttons & BMVR_IN_DUCK, "stick down crouches");
    i.stick[1][0] = 0.6f; i.stick[1][1] = -0.9f; c = step(&m, &i, 11.0f);
    CHECK(c.buttons & BMVR_IN_DUCK, "a started crouch survives sliding along the bottom edge");
    memset(&i.stick, 0, sizeof i.stick); c = step(&m, &i, 11.0f);
    CHECK(!(c.buttons & BMVR_IN_DUCK), "centre releases the crouch");
    i.stick[1][0] = 0.9f; c = step(&m, &i, 11.0f); i.stick[1][0] = 0.1f; i.stick[1][1] = 0.9f; c = step(&m, &i, 11.0f);
    CHECK(!(c.buttons & BMVR_IN_JUMP), "swinging past vertical after a turn does not jump");

    /* a control already held when the first state arrives is not an edge */
    struct bmvr_input_map m2; bmvr_input_map_init(&m2);
    struct bmvr_xfer_input h = mk(); h.buttons = LB(BMVR_XFER_BTN_MENU) | RB(BMVR_XFER_BTN_STICK); h.squeeze[1] = 1.0f;
    c = step(&m2, &h, 11.0f);
    CHECK(c.events == 0, "first state records, fires nothing (events %#x)", c.events);

    /* menu owns the controls */
    struct bmvr_input_map m3; bmvr_input_map_init(&m3); m3.cfg.menu_open = 1;
    struct bmvr_xfer_input q = mk(); c = step(&m3, &q, 11.0f);
    q.trigger[1] = 1.0f; q.stick[0][1] = 1.0f; c = step(&m3, &q, 11.0f);
    CHECK((c.events & BMVR_EV_MENU_SELECT) && c.buttons == 0 && c.forward == 0.0f, "menu: trigger clicks, nothing reaches the game");
    c = step(&m3, &q, 11.0f); CHECK(!(c.events & BMVR_EV_MENU_SELECT), "menu: held trigger clicks once");
    q.trigger[1] = 0.0f; q.buttons = RB(BMVR_XFER_BTN_SECONDARY); c = step(&m3, &q, 11.0f);
    CHECK(c.events == BMVR_EV_MENU_BACK, "menu: B goes back");
    q.buttons = RB(BMVR_XFER_BTN_PRIMARY); c = step(&m3, &q, 11.0f);
    CHECK(c.events == BMVR_EV_MENU_SELECT, "menu: A confirms");

    /* no input at all (helper offers none) is a quiet, valid state */
    c = step(&m3, &(struct bmvr_xfer_input){ 0 }, 11.0f);
    struct bmvr_input_map m4; bmvr_input_map_init(&m4); bmvr_input_map_step(&m4, NULL, 11.0f, &c);
    CHECK(c.buttons == 0 && c.events == 0 && c.forward == 0.0f, "NULL input is quiet");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}