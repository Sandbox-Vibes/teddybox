#include "ear_gestures.h"

eg_action_t ear_gesture_step(ear_gesture_t *g, int64_t now_us, bool ear_big, bool ear_small, bool upside_down)
{
    if (ear_big && ear_small)
    {
        if (!g->both_since)
        {
            g->both_since = now_us ? now_us : 1;
            g->level = 0;
            return EG_NONE;
        }
        int64_t held = now_us - g->both_since;
        if (g->level < 0)
        {
            return EG_NONE;
        }
        if ((g->level > 0 && !upside_down) || held >= EG_CANCEL_US)
        {
            g->level = -1;
            return EG_CANCELLED;
        }
        if (upside_down && g->level < 2 && held >= EG_OFW_US)
        {
            g->level = 2;
            return EG_OFW_ARMED;
        }
        if (upside_down && g->level < 1 && held >= EG_RESTART_US)
        {
            g->level = 1;
            return EG_RESTART_ARMED;
        }
        return EG_NONE;
    }

    if (!g->both_since)
    {
        return EG_NONE;
    }
    /* let go (one ear is enough) */
    int64_t held = now_us - g->both_since;
    int level = g->level;
    g->both_since = 0;
    g->level = 0;
    if (level == 2)
    {
        return EG_OFW;
    }
    if (level == 1)
    {
        return EG_RESTART;
    }
    if (level == 0 && held < EG_POWEROFF_MAX_US)
    {
        return EG_POWEROFF;
    }
    return EG_NONE;
}
