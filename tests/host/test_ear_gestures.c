#include <stdio.h>
#include "ear_gestures.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define S 1000000LL

/* hold both ears from t0 to t1 in 100 ms steps; returns the last non-NONE action while held, *rel the action on letting go */
static eg_action_t hold(ear_gesture_t *g, int64_t t0, int64_t t1, bool upside, eg_action_t *rel)
{
    eg_action_t last = EG_NONE;
    for (int64_t t = t0; t <= t1; t += S / 10)
    {
        eg_action_t a = ear_gesture_step(g, t, true, true, upside);
        if (a != EG_NONE) last = a;
    }
    *rel = ear_gesture_step(g, t1 + S / 10, false, false, upside);
    return last;
}

int main(void)
{
    ear_gesture_t g = {0};
    eg_action_t rel, a;

    /* short press: power off, upright or upside down */
    a = hold(&g, 100 * S, 101 * S, false, &rel);
    CHECK(a == EG_NONE && rel == EG_POWEROFF);
    a = hold(&g, 200 * S, 201 * S, true, &rel);
    CHECK(rel == EG_POWEROFF);
    /* 3 s or upright 15 s: nothing */
    a = hold(&g, 300 * S, 303 * S, false, &rel);
    CHECK(a == EG_NONE && rel == EG_NONE);
    a = hold(&g, 400 * S, 415 * S, false, &rel);
    CHECK(a == EG_NONE && rel == EG_NONE);
    /* upside down 12 s: restart on letting go */
    a = hold(&g, 500 * S, 512 * S, true, &rel);
    CHECK(a == EG_RESTART_ARMED && rel == EG_RESTART);
    /* upside down 22 s: original firmware on letting go */
    a = hold(&g, 600 * S, 622 * S, true, &rel);
    CHECK(a == EG_OFW_ARMED && rel == EG_OFW);
    /* 31 s: cancelled */
    a = hold(&g, 700 * S, 731 * S, true, &rel);
    CHECK(a == EG_CANCELLED && rel == EG_NONE);
    /* armed, then turned upright while holding: cancelled */
    g = (ear_gesture_t){0};
    for (int64_t t = 800 * S; t <= 811 * S; t += S / 10) ear_gesture_step(&g, t, true, true, true);
    CHECK(g.level == 1);
    CHECK(ear_gesture_step(&g, 811 * S + 1, true, true, false) == EG_CANCELLED);
    CHECK(ear_gesture_step(&g, 812 * S, false, false, false) == EG_NONE);
    /* one ear only: nothing */
    CHECK(ear_gesture_step(&g, 900 * S, true, false, true) == EG_NONE && ear_gesture_step(&g, 901 * S, false, false, true) == EG_NONE);

    printf("ear_gestures: %s\n", fails ? "FAILED" : "all tests ok");
    return fails != 0;
}
