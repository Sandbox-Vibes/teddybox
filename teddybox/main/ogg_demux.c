/* Minimal streaming Ogg demuxer, see ogg_demux.h */
#include <string.h>
#include "ogg_demux.h"

void ogg_demux_reset(ogg_demux_t *d)
{
    d->header_len = 0;
    d->header_need = 27;
    d->segment = -1;
    d->segment_left = 0;
    d->packet_len = 0;
    d->packet_valid = false;
    d->overflow = false;
    d->seqno_known = false;
}

void ogg_demux_init(ogg_demux_t *d)
{
    memset(d, 0, sizeof(*d));
    ogg_demux_reset(d);
}

static void segment_start(ogg_demux_t *d)
{
    d->segment_left = d->header[27 + d->segment];
}

/* a lacing value below 255 ends the packet */
static void segment_end(ogg_demux_t *d, ogg_packet_cb_t cb, void *ctx)
{
    if (d->header[27 + d->segment] < 255)
    {
        if (d->packet_valid && !d->overflow && d->packet_len)
        {
            cb(ctx, d->packet, d->packet_len);
        }
        else if (!d->packet_valid || d->overflow)
        {
            d->dropped++;
        }
        d->packet_len = 0;
        d->packet_valid = true;
        d->overflow = false;
    }
}

static void next_segment(ogg_demux_t *d, ogg_packet_cb_t cb, void *ctx)
{
    int segments = d->header[26];
    while (d->segment < segments && d->segment_left == 0)
    {
        if (d->segment >= 0)
        {
            segment_end(d, cb, ctx);
        }
        d->segment++;
        if (d->segment < segments)
        {
            segment_start(d);
        }
    }
    if (d->segment >= segments)
    {
        /* page done */
        d->header_len = 0;
        d->header_need = 27;
        d->segment = -1;
    }
}

void ogg_demux_feed(ogg_demux_t *d, const uint8_t *data, size_t len, ogg_packet_cb_t cb, void *ctx)
{
    while (len)
    {
        if (d->segment < 0)
        {
            /* page header */
            size_t n = d->header_need - d->header_len;
            if (n > len)
            {
                n = len;
            }
            memcpy(d->header + d->header_len, data, n);
            d->header_len += n;
            data += n;
            len -= n;
            if (d->header_len >= 4 && memcmp(d->header, "OggS", 4) != 0)
            {
                /* lost sync: drop one byte and search again */
                memmove(d->header, d->header + 1, d->header_len - 1);
                d->header_len--;
                d->sync_lost++;
                d->packet_valid = false;
                continue;
            }
            if (d->header_len < d->header_need)
            {
                continue;
            }
            if (d->header_need == 27)
            {
                if (d->header[4] != 0)
                {
                    /* unknown version: not a page, resync */
                    memmove(d->header, d->header + 1, 26);
                    d->header_len = 26;
                    d->sync_lost++;
                    continue;
                }
                d->header_need = 27 + d->header[26];
                if (d->header_len < d->header_need)
                {
                    continue;
                }
            }
            /* a jump in the page numbers (seek): a started packet is incomplete */
            uint32_t seqno = d->header[18] | d->header[19] << 8 | d->header[20] << 16 | (uint32_t)d->header[21] << 24;
            if (d->seqno_known && seqno != d->seqno && d->packet_len)
            {
                d->packet_len = 0;
                d->packet_valid = false;
                d->dropped++;
            }
            d->seqno = seqno + 1;
            d->seqno_known = true;
            /* continued packet on this page: only usable if we have its start */
            bool continued = d->header[5] & 1;
            if (!continued)
            {
                if (d->packet_len)
                {
                    d->dropped++;
                }
                d->packet_len = 0;
                d->packet_valid = true;
                d->overflow = false;
            }
            else if (!d->packet_len)
            {
                d->packet_valid = false;
            }
            d->segment = 0;
            if (d->header[26] == 0)
            {
                d->segment = 0;
                next_segment(d, cb, ctx);
                continue;
            }
            segment_start(d);
            next_segment(d, cb, ctx);
            continue;
        }

        /* page body */
        size_t n = d->segment_left < len ? d->segment_left : len;
        if (d->packet_valid && !d->overflow)
        {
            if (d->packet_len + n <= OGG_MAX_PACKET)
            {
                memcpy(d->packet + d->packet_len, data, n);
                d->packet_len += n;
            }
            else
            {
                d->overflow = true;
            }
        }
        data += n;
        len -= n;
        d->segment_left -= n;
        next_segment(d, cb, ctx);
    }
}
