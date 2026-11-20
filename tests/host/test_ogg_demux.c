/* ogg_demux: packets across pages, jumps in the page numbers (seek), garbage
   before a page, fed in chunks of every size */
#include <stdio.h>
#include <string.h>
#include "ogg_demux.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static size_t page(uint8_t *out, uint32_t seq, bool continued, const uint8_t *lacing, int nseg, const uint8_t *body, size_t blen)
{
    memset(out, 0, 27);
    memcpy(out, "OggS", 4);
    out[5] = continued ? 1 : 0;
    out[18] = seq; out[19] = seq >> 8;
    out[26] = nseg;
    memcpy(out + 27, lacing, nseg);
    memcpy(out + 27 + nseg, body, blen);
    return 27 + nseg + blen;
}

static size_t got_len[16];
static uint8_t got_first[16];
static int got;
static void cb(void *ctx, const uint8_t *p, size_t n) { (void)ctx; got_len[got] = n; got_first[got] = p[0]; got++; }

static void feed(ogg_demux_t *d, const uint8_t *s, size_t n, size_t chunk)
{
    for (size_t i = 0; i < n; i += chunk) ogg_demux_feed(d, s + i, n - i < chunk ? n - i : chunk, cb, NULL);
}

int main(void)
{
    static uint8_t stream[8192], body[1024];
    for (int i = 0; i < 1024; i++) body[i] = i / 100 + 1;
    for (size_t chunk = 1; chunk <= 700; chunk = chunk * 3 + 1)
    {
        size_t n = 0;
        /* page 0: packet A (100), start of packet B (255 + 255 = 510 so far) */
        uint8_t l0[] = {100, 255, 255};
        n += page(stream + n, 0, false, l0, 3, body, 610);
        /* page 1: rest of B (40), packet C (200) */
        uint8_t l1[] = {40, 200};
        n += page(stream + n, 1, true, l1, 2, body + 610, 240);
        /* garbage, then page 5 (jump): continued rest (30) must be dropped, then D (50) */
        memcpy(stream + n, "xxOgg", 5); n += 5;
        uint8_t l5[] = {30, 50};
        n += page(stream + n, 5, true, l5, 2, body + 850, 80);
        /* page 6: E (10) */
        uint8_t l6[] = {10};
        n += page(stream + n, 6, false, l6, 1, body + 930, 10);
        ogg_demux_t d;
        ogg_demux_init(&d);
        got = 0;
        feed(&d, stream, n, chunk);
        CHECK(got == 5);
        CHECK(got_len[0] == 100 && got_len[1] == 550 && got_len[2] == 200 && got_len[3] == 50 && got_len[4] == 10);
        CHECK(got_first[3] == body[880]);
        CHECK(d.sync_lost == 5 && d.dropped == 1);
    }
    /* packet started before a jump, continued page after it with a different number */
    {
        size_t n = 0;
        uint8_t l0[] = {255};
        n += page(stream + n, 0, false, l0, 1, body, 255);
        uint8_t l9[] = {20, 60};
        n += page(stream + n, 9, true, l9, 2, body + 300, 80);
        ogg_demux_t d;
        ogg_demux_init(&d);
        got = 0;
        feed(&d, stream, n, 64);
        CHECK(got == 1 && got_len[0] == 60);
    }
    printf("ogg_demux: %s\n", fails ? "FAILED" : "all tests ok");
    return fails != 0;
}
