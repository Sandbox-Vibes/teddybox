#include <string.h>
#include "rtnl_encode.h"

static size_t put_varint(uint8_t *p, uint64_t v)
{
    size_t n = 0;
    do
    {
        p[n] = v & 0x7f;
        v >>= 7;
        if (v)
        {
            p[n] |= 0x80;
        }
        n++;
    } while (v);
    return n;
}

static size_t put_le(uint8_t *p, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
    {
        p[i] = v >> (8 * i);
    }
    return bytes;
}

/* wraps the inner message (already at out + 4 + 2) as field `field` of the RPC
   and puts the frame length in front; inner messages stay below 128 bytes */
static size_t frame(uint8_t *out, int field, size_t inner_len)
{
    out[4] = (field << 3) | 2;
    out[5] = inner_len;
    size_t len = 2 + inner_len;
    out[0] = out[1] = 0;
    out[2] = len >> 8;
    out[3] = len;
    return 4 + len;
}

size_t rtnl_encode_log2(uint8_t *out, uint64_t uptime_ms, uint32_t sequence, uint32_t group, uint32_t function,
                        const uint8_t *payload, size_t len)
{
    if (len > RTNL_PAYLOAD_MAX)
    {
        len = RTNL_PAYLOAD_MAX;
    }
    uint8_t *p = out + 6;
    size_t n = 0;
    p[n++] = (1 << 3) | 1; /* uptime, fixed64 */
    n += put_le(p + n, uptime_ms, 8);
    p[n++] = 2 << 3; /* sequence */
    n += put_varint(p + n, sequence);
    p[n++] = 3 << 3; /* field3 */
    n += put_varint(p + n, 0);
    p[n++] = 4 << 3; /* function group */
    n += put_varint(p + n, group);
    p[n++] = 5 << 3; /* function */
    n += put_varint(p + n, function);
    p[n++] = (6 << 3) | 2; /* payload */
    n += put_varint(p + n, len);
    memcpy(p + n, payload, len);
    n += len;
    return frame(out, 2, n);
}

size_t rtnl_encode_log3(uint8_t *out, uint32_t datetime, uint32_t type)
{
    uint8_t *p = out + 6;
    size_t n = 0;
    p[n++] = (1 << 3) | 5; /* datetime, fixed32 */
    n += put_le(p + n, datetime, 4);
    p[n++] = 2 << 3;
    n += put_varint(p + n, type);
    return frame(out, 3, n);
}

void rtnl_payload_uid(uint8_t out[8], uint64_t uid)
{
    /* teddyCloud reads it with read_big_endian64(): low 32 bits first, each
       half big endian */
    for (int i = 0; i < 4; i++)
    {
        out[i] = uid >> (24 - 8 * i);
        out[4 + i] = uid >> (56 - 8 * i);
    }
}

void rtnl_payload_u32le(uint8_t out[4], uint32_t v) { put_le(out, v, 4); }

void rtnl_payload_volume(uint8_t out[12], int32_t dac_half_db, int32_t level)
{
    put_le(out, 0, 4);
    put_le(out + 4, (uint32_t)dac_half_db, 4);
    put_le(out + 8, (uint32_t)level, 4);
}
