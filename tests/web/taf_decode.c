/* Decodes a TAF with the box's own decoder pieces (CELT-only libopus build of
   components/opus_celt, ogg_demux.c) and writes 48 kHz stereo WAV:
   taf_decode in.taf out.wav. Counts packets the box would skip. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opus.h"
#include "ogg_demux.h"
static OpusDecoder *dec;
static int preskip, packets, bad, nonceltp;
static FILE *out;
static long frames;
static void on_packet(void *c, const unsigned char *p, size_t n)
{
    if (n >= 8 && !memcmp(p, "OpusHead", 8)) { preskip = p[10] | p[11] << 8; return; }
    if (n >= 8 && !memcmp(p, "OpusTags", 8)) return;
    packets++;
    if ((p[0] >> 3) < 16) nonceltp++;
    static opus_int16 pcm[2880 * 2];
    int s = opus_decode(dec, p, n, pcm, 2880, 0);
    if (s < 0) { bad++; return; }
    int skip = preskip < s ? preskip : s; preskip -= skip;
    fwrite(pcm + skip * 2, 4, s - skip, out);
    frames += s - skip;
}
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) return 1;
    out = fopen(argv[2], "wb");
    unsigned char h[44] = {0}; fwrite(h, 1, 44, out);
    int e; dec = opus_decoder_create(48000, 2, &e);
    static ogg_demux_t d; ogg_demux_init(&d);
    for (long o = 4096; o < n; o += 4096) ogg_demux_feed(&d, b + o, n - o < 4096 ? n - o : 4096, on_packet, 0);
    long bytes = frames * 4;
    memcpy(h, "RIFF", 4); *(int *)(h + 4) = 36 + bytes; memcpy(h + 8, "WAVEfmt ", 8); *(int *)(h + 16) = 16;
    *(short *)(h + 20) = 1; *(short *)(h + 22) = 2; *(int *)(h + 24) = 48000; *(int *)(h + 28) = 192000;
    *(short *)(h + 32) = 4; *(short *)(h + 34) = 16; memcpy(h + 36, "data", 4); *(int *)(h + 40) = bytes;
    fseek(out, 0, SEEK_SET); fwrite(h, 1, 44, out); fclose(out);
    printf("packets %d, decode errors %d, non-CELT %d, ogg sync lost %u, dropped %u, %.3f s\n",
           packets, bad, nonceltp, d.sync_lost, d.dropped, frames / 48000.0);
    return bad || nonceltp || d.sync_lost || d.dropped;
}
