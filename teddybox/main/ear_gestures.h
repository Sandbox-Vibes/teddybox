#pragma once
/*
 * Gestures with both ears, aligned with the original firmware and the
 * Hackiebox CFW so that parents with several boxes use the same gestures:
 *   - both ears pressed and let go within 2 s: power off (Hackiebox CFW)
 *   - box upside down, both ears held 10 s, let go: restart (as original)
 *   - box upside down, both ears held 20 s, let go: original firmware
 * The long gestures act only when the ears are let go, so the firmware
 * that starts next never sees the ears still held upside down (in the
 * original that is restart, on the charger factory reset). Held 30 s:
 * nothing. Turning the box upright while holding cancels.
 * Plain C without ESP-IDF, unit tested on a PC.
 */
#include <stdbool.h>
#include <stdint.h>

#define EG_POWEROFF_MAX_US (2 * 1000000LL)
#define EG_RESTART_US (10 * 1000000LL)
#define EG_OFW_US (20 * 1000000LL)
#define EG_CANCEL_US (30 * 1000000LL)

typedef enum
{
    EG_NONE,
    EG_RESTART_ARMED, /* 10 s reached: signal, restart when let go */
    EG_OFW_ARMED,     /* 20 s reached: signal, original firmware when let go */
    EG_CANCELLED,     /* held too long or turned upright: nothing when let go */
    EG_POWEROFF,
    EG_RESTART,
    EG_OFW,
} eg_action_t;

typedef struct
{
    int64_t both_since; /* 0 = not both pressed */
    int level;          /* 0 nothing, 1 restart armed, 2 original armed, -1 cancelled */
} ear_gesture_t;

/* call regularly; upside_down: box upside down (true without a working
   accelerometer, so that the way back to the original firmware stays) */
eg_action_t ear_gesture_step(ear_gesture_t *g, int64_t now_us, bool ear_big, bool ear_small, bool upside_down);
