#pragma once

/*
 * Freshness check, like the original firmware does it:
 * POST /v1/freshness-check with UID and audio_id of every tonie on the SD
 * card. The response lists the UIDs whose content changed (new version on
 * teddyCloud / in the Boxine cloud, live content). Their files are moved to
 * "<file>.old", so the next time the tonie is placed the new version is
 * downloaded. If that download fails, playback falls back to the .old file.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define FRESHNESS_INTERVAL_MS (6 * 60 * 60 * 1000)

typedef struct
{
    /* box settings configured in teddyCloud, -1 if not sent */
    int max_vol_spk;
    int max_vol_hdp;
    int slap_en;
    int slap_dir;
    int led;
} freshness_settings_t;

/* what the box changes after teddyCloud sent its box settings; -1 = no change */
typedef struct
{
    int volume_max_spk; /* percent */
    int volume_max_hdp; /* percent */
    int led_brightness; /* 0..3 */
    int slap_level;     /* 0..5 */
    int slap_reverse;   /* 0/1 */
} freshness_box_changes_t;

/* Only values that changed in teddyCloud since the last check are taken
   over (last == NULL: first check, nothing is taken, it is the baseline).
   A setting changed on the box itself stays until it is changed in
   teddyCloud (or the Tonie app, passed through by teddyCloud) again.
   cur_slap_level: slap switched on again goes back to 3 if it was off.
   Returns the number of changes. */
int freshness_box_changes(const freshness_settings_t *now, const freshness_settings_t *last, int cur_slap_level,
                          freshness_box_changes_t *out);

/* box settings received: applied by main.c (control.h), stored in NVS */
void freshness_box_settings_received(const freshness_settings_t *s);

/* runs one check, call from the cloud task only (uses cloud_post) */
esp_err_t freshness_run(void);

/* helpers, exposed for unit tests */

/* UID from a content path "XXXXXXXX/YYYYYYYY" (dir, file); false if no tonie file */
bool freshness_uid_from_names(const char *dir, const char *file, uint64_t *uid);
/* number of completed freshness checks since start */
int freshness_runs(void);

/* parses a TonieFreshnessCheckResponse; returns number of UIDs stored in marked, -1 on error */
int freshness_parse_response(const uint8_t *buf, size_t len, uint64_t *marked, int max_marked, freshness_settings_t *settings);
