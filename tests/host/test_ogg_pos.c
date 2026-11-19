/* Ogg page position helpers (playback progress for MQTT).
 *   ./test_ogg_pos            unit tests with synthetic pages
 *   ./test_ogg_pos FILE...    real TAF files: duration and block check */
#include <stdio.h>
#include <string.h>
#include "ogg_pos.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void page(uint8_t *p, uint64_t granule)
{
    memset(p, 0, 27);
    memcpy(p, "OggS", 4);
    for (int i = 0; i < 8; i++) p[6 + i] = granule >> (8 * i);
}

int main(int argc, char **argv)
{
    if (argc > 1)
    {
        static uint8_t buf[4096];
        for (int a = 1; a < argc; a++)
        {
            FILE *f = fopen(argv[a], "rb");
            if (!f) { printf("FAIL open %s\n", argv[a]); return 1; }
            uint64_t total = pb_ogg_total_granule(f, buf, sizeof buf);
            /* every audio block (after the 4 KB header) must start with a page */
            fseek(f, 0, SEEK_END); long size = ftell(f);
            int blocks = 0, bad = 0; uint64_t last = 0; int monotonic = 1;
            for (long off = 4096; off < size; off += 4096, blocks++)
            {
                fseek(f, off, SEEK_SET);
                int n = fread(buf, 1, 27, f);
                uint64_t g = pb_ogg_granule(buf, n);
                if (g == PB_NO_GRANULE) bad++;
                else { if (blocks > 1 && g < last) monotonic = 0; last = g; }
            }
            fclose(f);
            printf("%s: duration %.3f s, %d blocks, %d without page, %s\n", argv[a], total / (double)PB_OPUS_RATE,
                   blocks, bad, monotonic ? "monotonic" : "NOT monotonic");
        }
        return 0;
    }

    uint8_t p[64];
    page(p, 48000 * 75);
    CHECK(pb_ogg_granule(p, sizeof p) == 48000ULL * 75, "granule");
    CHECK(pb_ogg_granule(p, 26) == PB_NO_GRANULE, "too short");
    p[4] = 1;
    CHECK(pb_ogg_granule(p, sizeof p) == PB_NO_GRANULE, "version");
    p[4] = 0; p[5] = 0x08;
    CHECK(pb_ogg_granule(p, sizeof p) == PB_NO_GRANULE, "flags");
    p[5] = 0x04; /* end of stream */
    CHECK(pb_ogg_granule(p, sizeof p) == 48000ULL * 75, "eos flag ok");
    p[0] = 'X';
    CHECK(pb_ogg_granule(p, sizeof p) == PB_NO_GRANULE, "magic");

    /* duration = last page, also if a page with granule -1 follows */
    static uint8_t file[3 * 4096];
    memset(file, 0x55, sizeof file);
    page(&file[4096], 48000);
    page(&file[8192], 48000 * 2);
    page(&file[8192 + 700], 48000 * 3);
    page(&file[8192 + 1500], PB_NO_GRANULE);
    FILE *f = tmpfile();
    fwrite(file, 1, sizeof file, f);
    static uint8_t scratch[4096];
    CHECK(pb_ogg_total_granule(f, scratch, sizeof scratch) == 48000ULL * 3, "total %llu",
          (unsigned long long)pb_ogg_total_granule(f, scratch, sizeof scratch));
    fclose(f);

    printf("ogg_pos: %s\n", fails ? "FAILED" : "all tests ok");
    return fails != 0;
}
