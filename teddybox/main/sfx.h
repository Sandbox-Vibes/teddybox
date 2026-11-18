#pragma once

#include <stdint.h>
#include "esp_err.h"

/* system sound container written by the original firmware into the assets partition */
#define SFX_BIN_PATH "/spiflash/fw/sfx.bin"

/*
 * sfx.bin is a plain concatenation of 25 TAF files, one per system sound ID
 * (CONTENT_DEFAULT_* in playback.h, same order). Unused IDs (4, 5, 12) are
 * stored as a 32 byte stub: 4 byte length + 28 byte header with num_bytes = 0.
 *
 * sfx_install() copies every non-empty entry to /sdcard/CONTENT/<lang>/<id>
 * unless such a file already exists, so user supplied sounds take precedence.
 */
esp_err_t sfx_install(uint8_t lang_id);
