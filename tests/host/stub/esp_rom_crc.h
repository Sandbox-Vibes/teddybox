#pragma once
#include <stdint.h>
#include <zlib.h>
/* host: zlib's crc32 is the same CRC-32 as esp_rom_crc32_le(0, ...) */
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len) { return crc32(crc, buf, len); }
