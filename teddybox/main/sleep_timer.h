#pragma once
/*
 * Sleep timer: after the set time the box stops and switches off. Over the
 * last third of the time (1 to 10 minutes) the volume goes down linearly.
 * Plain C without ESP-IDF, unit tested on a PC; main.c drives it.
 */
#include <stdbool.h>
#include <stdint.h>

#define SLEEP_TIMER_MAX_MIN 240
#define SLEEP_TIMER_FADE_MIN_US (60LL * 1000000)
#define SLEEP_TIMER_FADE_MAX_US (10LL * 60 * 1000000)

typedef struct
{
    int64_t start_us;
    int64_t end_us; /* 0 = off */
} sleep_timer_t;

/* minutes 1..SLEEP_TIMER_MAX_MIN start (or restart) it, 0 switches it off */
void sleep_timer_set(sleep_timer_t *t, int64_t now_us, int minutes);
bool sleep_timer_active(const sleep_timer_t *t);
/* minutes left, rounded up; 0 when off */
int sleep_timer_remaining_min(const sleep_timer_t *t, int64_t now_us);
/* volume factor in 1/1000: 1000 before the fade, then down to 0;
   -1 when the time is up (stop and switch off) */
int sleep_timer_level(const sleep_timer_t *t, int64_t now_us);
