/* ZIP archive without compression ("stored"), enough for a few small files */
#include <string.h>
#include "esp_rom_crc.h"
#include "backup_zip.h"

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

int backup_zip(const char *const names[], const uint8_t *const data[], const size_t sizes[], int count, uint8_t *out,
               size_t out_size)
{
    size_t pos = 0, central_start;
    uint32_t offsets[8];
    if (count > 8)
    {
        return -1;
    }
    /* local headers with the data */
    for (int i = 0; i < count; i++)
    {
        size_t nlen = strlen(names[i]);
        if (pos + 30 + nlen + sizes[i] > out_size)
        {
            return -1;
        }
        uint8_t *h = out + pos;
        uint32_t crc = esp_rom_crc32_le(0, data[i], sizes[i]);
        offsets[i] = pos;
        put32(h, 0x04034b50);
        put16(h + 4, 10);     /* version needed */
        put16(h + 6, 0);      /* flags */
        put16(h + 8, 0);      /* stored */
        put16(h + 10, 0);     /* time */
        put16(h + 12, 0x21);  /* date 1980-01-01 */
        put32(h + 14, crc);
        put32(h + 18, sizes[i]);
        put32(h + 22, sizes[i]);
        put16(h + 26, nlen);
        put16(h + 28, 0);
        memcpy(h + 30, names[i], nlen);
        memcpy(h + 30 + nlen, data[i], sizes[i]);
        pos += 30 + nlen + sizes[i];
    }
    /* central directory */
    central_start = pos;
    for (int i = 0; i < count; i++)
    {
        size_t nlen = strlen(names[i]);
        if (pos + 46 + nlen > out_size)
        {
            return -1;
        }
        const uint8_t *l = out + offsets[i];
        uint8_t *c = out + pos;
        memset(c, 0, 46);
        put32(c, 0x02014b50);
        put16(c + 4, 20);              /* made by */
        memcpy(c + 6, l + 4, 26);      /* version needed .. name length */
        put32(c + 42, offsets[i]);
        memcpy(c + 46, names[i], nlen);
        pos += 46 + nlen;
    }
    if (pos + 22 > out_size)
    {
        return -1;
    }
    uint8_t *e = out + pos;
    memset(e, 0, 22);
    put32(e, 0x06054b50);
    put16(e + 8, count);
    put16(e + 10, count);
    put32(e + 12, pos - central_start);
    put32(e + 16, central_start);
    return pos + 22;
}

