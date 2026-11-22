/* RTNL frame encoding: decoded again field by field, as teddyCloud's
   protobuf-c parser sees it (TonieRtnlRPC: log2 = field 2, log3 = field 3;
   all required fields present) */
#include <stdio.h>
#include <string.h>
#include "rtnl_encode.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

typedef struct
{
    int seen[10];
    uint64_t v[10];
    const uint8_t *bytes;
    size_t bytes_len;
} msg_t;

static int varint(const uint8_t *p, size_t len, size_t *pos, uint64_t *v)
{
    *v = 0;
    for (int s = 0; s < 64 && *pos < len; s += 7)
    {
        uint8_t b = p[(*pos)++];
        *v |= (uint64_t)(b & 0x7f) << s;
        if (!(b & 0x80))
        {
            return 1;
        }
    }
    return 0;
}

static int parse(const uint8_t *p, size_t len, msg_t *m)
{
    memset(m, 0, sizeof(*m));
    size_t pos = 0;
    while (pos < len)
    {
        uint64_t key, v = 0;
        if (!varint(p, len, &pos, &key))
        {
            return 0;
        }
        int f = key >> 3, t = key & 7;
        if (f >= 10)
        {
            return 0;
        }
        if (t == 0)
        {
            if (!varint(p, len, &pos, &v))
            {
                return 0;
            }
        }
        else if (t == 1 || t == 5)
        {
            int n = t == 1 ? 8 : 4;
            if (pos + n > len)
            {
                return 0;
            }
            for (int i = 0; i < n; i++)
            {
                v |= (uint64_t)p[pos + i] << (8 * i);
            }
            pos += n;
        }
        else if (t == 2)
        {
            if (!varint(p, len, &pos, &v) || pos + v > len)
            {
                return 0;
            }
            m->bytes = p + pos;
            m->bytes_len = v;
            pos += v;
        }
        else
        {
            return 0;
        }
        m->seen[f]++;
        m->v[f] = v;
    }
    return 1;
}

/* frame -> inner message of field `field` */
static int unframe(const uint8_t *f, size_t n, int field, msg_t *inner)
{
    uint32_t len = (uint32_t)f[0] << 24 | f[1] << 16 | f[2] << 8 | f[3];
    if (f[0] || f[1] || len + 4 != n)
    {
        return 0;
    }
    msg_t rpc;
    if (!parse(f + 4, len, &rpc) || rpc.seen[field] != 1 || rpc.seen[field == 2 ? 3 : 2])
    {
        return 0;
    }
    return parse(rpc.bytes, rpc.bytes_len, inner);
}

int main(void)
{
    uint8_t f[RTNL_FRAME_MAX], p[12];
    msg_t m;

    /* tag valid: as teddyCloud's read_big_endian64() reads it, low half first */
    rtnl_payload_uid(p, 0xE0040350D4C3B2A1ULL);
    size_t n = rtnl_encode_log2(f, 123456789012ULL, 300, RTNL2_FUGR_TAG, RTNL2_FUNC_TAG_VALID, p, 8);
    CHECK(n <= RTNL_FRAME_MAX && unframe(f, n, 2, &m));
    for (int i = 1; i <= 6; i++)
    {
        CHECK(m.seen[i] == 1); /* required fields of TonieRtnlLog2 */
    }
    CHECK(m.v[1] == 123456789012ULL && m.v[2] == 300 && m.v[3] == 0 && m.v[4] == 15 && m.v[5] == 16065);
    CHECK(m.bytes_len == 8 && !memcmp(m.bytes, "\xd4\xc3\xb2\xa1\xe0\x04\x03\x50", 8));

    /* volume: like the original "D8FFFFFF 00000000" for level 0, -20 dB */
    rtnl_payload_volume(p, -40, 0);
    n = rtnl_encode_log2(f, 1, 1, RTNL2_FUGR_VOLUME, RTNL2_FUNC_VOLUME_CHANGE, p, 12);
    CHECK(unframe(f, n, 2, &m) && m.v[4] == 27 && m.v[5] == 15524 && m.bytes_len == 12);
    CHECK(!memcmp(m.bytes + 4, "\xd8\xff\xff\xff\x00\x00\x00\x00", 8));

    /* audio id little endian */
    rtnl_payload_u32le(p, 0x5F1A2B3C);
    n = rtnl_encode_log2(f, 1, 2, RTNL2_FUGR_AUDIO_A, RTNL2_FUNC_AUDIO_ID, p, 4);
    CHECK(unframe(f, n, 2, &m) && m.v[4] == 20 && m.v[5] == 16096 && !memcmp(m.bytes, "\x3c\x2b\x1a\x5f", 4));

    /* longest payload still fits, longer is cut */
    static uint8_t big[100];
    n = rtnl_encode_log2(f, ~0ULL, ~0U, RTNL2_FUGR_FIRMWARE, RTNL2_FUNC_FIRMWARE_VERSION, big, sizeof(big));
    CHECK(n <= RTNL_FRAME_MAX && unframe(f, n, 2, &m) && m.bytes_len == RTNL_PAYLOAD_MAX && m.v[2] == 0xFFFFFFFFu);

    /* log3 */
    n = rtnl_encode_log3(f, 1763800000, RTNL3_PLAYBACK_STARTED);
    CHECK(unframe(f, n, 3, &m) && m.seen[1] == 1 && m.seen[2] == 1 && m.v[1] == 1763800000 && m.v[2] == 12);

    printf("rtnl: %s\n", fails ? "FAILED" : "all tests ok");
    return fails != 0;
}
