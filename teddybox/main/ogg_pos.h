#pragma once

/* playback position from Ogg page headers (every TAF block starts with a page) */

#include <stdint.h>
#include <stdio.h>

#define PB_NO_GRANULE UINT64_MAX
#define PB_OPUS_RATE 48000

/* granule position (48 kHz samples) of the Ogg page header at buf, PB_NO_GRANULE if none */
uint64_t pb_ogg_granule(const uint8_t *buf, int len);
/* granule of the last page in the file = duration, 0 if not found; uses buf as scratch */
uint64_t pb_ogg_total_granule(FILE *fd, uint8_t *buf, size_t buf_size);
