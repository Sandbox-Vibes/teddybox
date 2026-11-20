#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opus.h"
#include "ogg_demux.h"
static OpusDecoder *dec; static int preskip; static unsigned hash = 2166136261u, bytes;
static void on_packet(void *c, const unsigned char *p, size_t n)
{
    if (n >= 8 && !memcmp(p, "OpusHead", 8)) { preskip = p[10] | p[11] << 8; opus_decoder_ctl(dec, OPUS_RESET_STATE); return; }
    if (n >= 8 && !memcmp(p, "OpusTags", 8)) return;
    static opus_int16 pcm[2880 * 2]; int s = opus_decode(dec, p, n, pcm, 2880, 0); if (s < 0) return;
    int skip = preskip < s ? preskip : s; preskip -= skip;
    unsigned char *b = (unsigned char *)(pcm + skip * 2);
    for (int i = 0; i < (s - skip) * 4; i++) hash = (hash ^ b[i]) * 16777619u;
    bytes += (s - skip) * 4;
}
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); static unsigned char b[1 << 20]; long n = fread(b, 1, sizeof b, f);
    long jf = argc > 2 ? atol(argv[2]) : 0, jt = argc > 3 ? atol(argv[3]) : 0;
    int e; dec = opus_decoder_create(48000, 2, &e); static ogg_demux_t d; ogg_demux_init(&d);
    if (jf) { ogg_demux_feed(&d, b, jf, on_packet, 0); ogg_demux_feed(&d, b + jt, n - jt, on_packet, 0); }
    else ogg_demux_feed(&d, b, n, on_packet, 0);
    printf("%u bytes PCM, hash %08x\n", bytes, hash);
}
