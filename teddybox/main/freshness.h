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

/* runs one check, call from the cloud task only (uses cloud_post) */
esp_err_t freshness_run(void);

/* helpers, exposed for unit tests */

/* UID from a content path "XXXXXXXX/YYYYYYYY" (dir, file); false if no tonie file */
bool freshness_uid_from_names(const char *dir, const char *file, uint64_t *uid);

/* parses a TonieFreshnessCheckResponse; returns number of UIDs stored in marked, -1 on error */
int freshness_parse_response(const uint8_t *buf, size_t len, uint64_t *marked, int max_marked, freshness_settings_t *settings);
