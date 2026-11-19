#pragma once

/*
 * Firmware updates of TeddyBox itself, uploaded through the setup page.
 * There is no network port for updates: the setup page only runs after the
 * big ear was held while switching on, so physical access is required.
 *
 * Updates go to a slot without an original Boxine app. With the stock layout
 * (PD app, TeddyBox, EU app) the first update needs the PD slot; after that
 * TeddyBox alternates between two slots and the EU app is never touched.
 *
 * Rollback: the original bootloader boots a new image as "pending verify".
 * TeddyBox confirms itself with ota_confirm_running() once it runs properly;
 * if it restarts before that, the bootloader goes back to the previous slot.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_ota_ops.h"

typedef struct
{
    const esp_partition_t *target;
    esp_ota_handle_t handle;
    size_t written;
    bool started;
    esp_app_desc_t new_app;
} ota_session_t;

/* checks an image header (first bytes of the file), returns NULL if fine or a message for the user */
const char *ota_check_header(const uint8_t *data, size_t len, size_t total_len, esp_app_desc_t *desc);

/* picks the target slot; allow_pd: the PD app may be overwritten if nothing else is free.
   Returns NULL on success or a message; *needs_pd is set if only the PD slot is left */
const char *ota_session_begin(ota_session_t *s, size_t image_len, bool allow_pd, bool *needs_pd);
const char *ota_session_write(ota_session_t *s, const uint8_t *data, size_t len);
/* validates the image and makes it the boot slot (boots as "pending verify") */
const char *ota_session_finish(ota_session_t *s);
void ota_session_abort(ota_session_t *s);

/* marks the running image as good, cancels a pending rollback; safe to call any time */
void ota_confirm_running(void);
