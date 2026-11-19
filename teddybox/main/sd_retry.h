#pragma once
#include <stdint.h>

/* The SD card on the box sometimes stops answering for 1-2 s (sdmmc 0x107,
   always on a read, with and without a download). FATFS then marks the open
   file as broken, so it has to be opened again. Waits before attempt 2..n;
   together about 3.7 s. */
#define SD_RETRIES 5
static inline uint32_t sd_retry_delay_ms(int attempt)
{
    static const uint16_t ms[SD_RETRIES] = {0, 200, 500, 1000, 2000};
    return attempt < SD_RETRIES ? ms[attempt] : 2000;
}
