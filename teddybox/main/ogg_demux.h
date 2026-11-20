#pragma once
/*
 * Minimal streaming Ogg demuxer for the Opus stream of a tonie file.
 * Fed with arbitrary chunks of the file (also after a seek to a page
 * boundary); calls back with each complete packet. A packet that started
 * before a seek (continued page without the beginning) is dropped.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* an Opus packet is at most 1275 bytes per 20 ms frame, 3 frames in 60 ms */
#define OGG_MAX_PACKET 4096

typedef void (*ogg_packet_cb_t)(void *ctx, const uint8_t *packet, size_t len);

typedef struct
{
    uint8_t header[27 + 255];
    size_t header_len;
    size_t header_need;  /* 27, then 27 + number of segments */
    int segment;         /* current segment of the page body, -1: in header */
    size_t segment_left; /* bytes left of the current segment */
    uint8_t packet[OGG_MAX_PACKET];
    size_t packet_len;
    bool packet_valid; /* false: dropping the rest of a packet */
    bool overflow;
    uint32_t seqno;     /* page sequence number expected next */
    bool seqno_known;
    uint32_t sync_lost; /* bytes skipped to find "OggS" */
    uint32_t dropped;   /* packets dropped (seek, overflow) */
} ogg_demux_t;

void ogg_demux_init(ogg_demux_t *d);
/* after a seek: the next data starts at a page boundary */
void ogg_demux_reset(ogg_demux_t *d);
void ogg_demux_feed(ogg_demux_t *d, const uint8_t *data, size_t len, ogg_packet_cb_t cb, void *ctx);
