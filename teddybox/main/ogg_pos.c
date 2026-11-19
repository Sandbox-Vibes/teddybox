#include <stdio.h>
#include <string.h>

#include "ogg_pos.h"

/* granule position of an Ogg page header at buf, PB_NO_GRANULE if none */
uint64_t pb_ogg_granule(const uint8_t *buf, int len)
{
    /* version 0, only the 3 defined header type flags */
    if (len < 27 || memcmp(buf, "OggS", 4) || buf[4] != 0 || (buf[5] & 0xF8))
    {
        return PB_NO_GRANULE;
    }
    uint64_t g = 0;
    for (int i = 7; i >= 0; i--)
    {
        g = (g << 8) | buf[6 + i];
    }
    return g; /* -1 (no packet ends on this page) is PB_NO_GRANULE as well */
}

/* duration: granule of the last complete Ogg page in the file */
uint64_t pb_ogg_total_granule(FILE *fd, uint8_t *buf, size_t buf_size)
{
    if (fseek(fd, 0, SEEK_END) != 0)
    {
        return 0;
    }
    long size = ftell(fd);
    long start = size > (long)buf_size ? size - (long)buf_size : 0;
    fseek(fd, start, SEEK_SET);
    int n = fread(buf, 1, buf_size, fd);
    for (int i = n - 27; i >= 0; i--)
    {
        uint64_t g = pb_ogg_granule(&buf[i], n - i);
        if (g != PB_NO_GRANULE && g != 0)
        {
            return g;
        }
    }
    return 0;
}

