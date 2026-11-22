#include "sleep_timer.h"

void sleep_timer_set(sleep_timer_t *t, int64_t now_us, int minutes)
{
    if (minutes <= 0)
    {
        t->start_us = t->end_us = 0;
        return;
    }
    if (minutes > SLEEP_TIMER_MAX_MIN)
    {
        minutes = SLEEP_TIMER_MAX_MIN;
    }
    t->start_us = now_us;
    t->end_us = now_us + (int64_t)minutes * 60 * 1000000;
    if (t->end_us == 0)
    {
        t->end_us = 1;
    }
}

bool sleep_timer_active(const sleep_timer_t *t) { return t->end_us != 0; }

int sleep_timer_remaining_min(const sleep_timer_t *t, int64_t now_us)
{
    if (!t->end_us)
    {
        return 0;
    }
    int64_t left = t->end_us - now_us;
    if (left <= 0)
    {
        return 0;
    }
    return (int)((left + 60LL * 1000000 - 1) / (60LL * 1000000));
}

int sleep_timer_level(const sleep_timer_t *t, int64_t now_us)
{
    if (!t->end_us)
    {
        return 1000;
    }
    int64_t left = t->end_us - now_us;
    if (left <= 0)
    {
        return -1;
    }
    int64_t fade = (t->end_us - t->start_us) / 3;
    if (fade < SLEEP_TIMER_FADE_MIN_US)
    {
        fade = SLEEP_TIMER_FADE_MIN_US;
    }
    if (fade > SLEEP_TIMER_FADE_MAX_US)
    {
        fade = SLEEP_TIMER_FADE_MAX_US;
    }
    if (left >= fade)
    {
        return 1000;
    }
    return (int)(left * 1000 / fade);
}
