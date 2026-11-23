#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * Nightly update: once a day (teddybox.ini update_hour, local time) the box
 * wakes up silently, asks teddyCloud which of its tonies have new content
 * (freshness check) and downloads the new versions and unfinished downloads,
 * so this does not happen while a tonie plays. On the charger always, on
 * battery only with at least MAINT_MIN_BATTERY_PERCENT. A pressed ear stops
 * it and starts the box normally.
 */
#define MAINT_MIN_BATTERY_PERCENT 50
/* give up on the whole run after this long */
#define MAINT_MAX_RUN_S (45 * 60)
/* a download without progress for this long is given up (resumed next time) */
#define MAINT_STALL_S 120
/* minimum time between the start of two runs */
#define MAINT_MIN_GAP_S 3600

/* very early in app_main, on a timer wake: is the update due now? */
bool maint_due(void);
/* when going to sleep: plan the next run (needs the clock and the settings) */
void maint_schedule_next(void);
/* microseconds until the next run, 0 if none is planned */
uint64_t maint_sleep_us(void);
/* the update itself; WiFi, cloud and battery must be started */
void maint_run(void);
/* extra time after MAINT_MAX_RUN_S before the guard switches the box off */
#define MAINT_GUARD_EXTRA_S (5 * 60)
/* update mode: switch off (and note the step) if the run hangs */
void maint_guard_start(void);
/* steps after maint_run, for the report of a run that hangs there */
void maint_stage(const char *what);
/* the run ended normally (right before switching off) */
void maint_run_end(void);
/* log line at start: last run with result, next planned run */
void maint_log_status(void);

/* the token of a tonie is needed to download its content later without the
   tonie being on the box; stored on the SD card when a tonie is placed */
void maint_token_save(uint64_t uid, const uint8_t token[32]);
bool maint_token_load(uint64_t uid, uint8_t token[32]);

/* token file TOKENS/<dir>/<file>, same names as the content in CONTENT/ */
void maint_token_names(uint64_t uid, char dir[9], char file[9]);

/* last run for Home Assistant: time (0 = never) and a short German text */
time_t maint_last(const char **result);

/* pure helpers (host tests) */
/* next local time hour:minute strictly after now, at least min_gap_s after last */
time_t maint_next_time(time_t now, int hour, int minute, time_t last, int min_gap_s);
