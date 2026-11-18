#pragma once

#include <stdbool.h>
#include "esp_partition.h"

/*
 * Knows which OTA slots hold the original Boxine firmware.
 *
 * The original firmware uses three app slots (ota_0..ota_2):
 *   "toniebox-esp32-eu"  the regular app
 *   "toniebox-esp32-pd"  production/fallback app; it takes over and demands an
 *                        update if the EU app is invalid ("Codewort Murmeltier")
 * and tracks them in NVS namespace TB_OTA (keys EU, PD, FREE).
 *
 * TeddyBox must never overwrite the EU app, and the PD app only on request.
 */

void bootsel_init(void);

/* partition with the original EU app, NULL if none found */
const esp_partition_t *bootsel_get_original(void);

/* true if the partition holds an original Boxine app */
bool bootsel_is_original(const esp_partition_t *part);

/* slot an OTA update of TeddyBox may be written to, NULL if none is safe */
const esp_partition_t *bootsel_get_update_target(bool allow_overwrite_pd);

/* set the original EU app as boot partition and restart, returns only on error */
esp_err_t bootsel_boot_original(void);
