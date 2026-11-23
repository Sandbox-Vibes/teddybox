#include <ctype.h>
#include <string.h>
#include "mdns_min.h"

/* reads a (possibly compressed) name at pos into "a.b.c", lower case;
   returns the position after it, or -1 */
static int read_name(const uint8_t *buf, int len, int pos, char *out, int out_size)
{
    int o = 0, jumps = 0, end = -1;
    while (pos >= 0 && pos < len)
    {
        uint8_t l = buf[pos];
        if (l == 0)
        {
            out[o] = 0;
            return end < 0 ? pos + 1 : end;
        }
        if ((l & 0xC0) == 0xC0)
        {
            if (pos + 1 >= len || ++jumps > 8)
            {
                return -1;
            }
            if (end < 0)
            {
                end = pos + 2;
            }
            pos = ((l & 0x3F) << 8) | buf[pos + 1];
            continue;
        }
        if ((l & 0xC0) || pos + 1 + l > len || o + l + 2 > out_size)
        {
            return -1;
        }
        if (o)
        {
            out[o++] = '.';
        }
        for (int i = 0; i < l; i++)
        {
            out[o++] = (char)tolower(buf[pos + 1 + i]);
        }
        pos += 1 + l;
    }
    return -1;
}

static int put_name(const char *host, uint8_t *p, int room)
{
    int l = (int)strlen(host);
    if (l < 1 || l > 63 || l + 8 > room)
    {
        return -1;
    }
    int n = 0;
    p[n++] = (uint8_t)l;
    memcpy(p + n, host, l);
    n += l;
    p[n++] = 5;
    memcpy(p + n, "local", 5);
    n += 5;
    p[n++] = 0;
    return n;
}

static int build(const char *host, uint32_t ip, uint16_t id, bool legacy, uint8_t *out, int out_size)
{
    if (out_size < 12)
    {
        return 0;
    }
    memset(out, 0, 12);
    out[0] = (uint8_t)(id >> 8);
    out[1] = (uint8_t)id;
    out[2] = 0x84; /* response, authoritative */
    out[5] = legacy ? 1 : 0;
    out[7] = 1;
    int n = 12, k;
    if (legacy)
    {
        if ((k = put_name(host, out + n, out_size - n)) < 0 || n + k + 4 > out_size)
        {
            return 0;
        }
        n += k;
        const uint8_t q[] = {0, 1, 0, 1};
        memcpy(out + n, q, 4);
        n += 4;
    }
    if ((k = put_name(host, out + n, out_size - n)) < 0 || n + k + 14 > out_size)
    {
        return 0;
    }
    n += k;
    /* A, class IN (+ cache flush, not for legacy), TTL 120 s, 4 bytes */
    const uint8_t rr[] = {0, 1, legacy ? 0x00 : 0x80, 1, 0, 0, 0, 120, 0, 4};
    memcpy(out + n, rr, sizeof(rr));
    n += sizeof(rr);
    memcpy(out + n, &ip, 4);
    return n + 4;
}

int mdns_min_answer(const uint8_t *in, int in_len, const char *host, uint32_t ip, bool legacy, uint8_t *out, int out_size)
{
    if (in_len < 12 || (in[2] & 0x80))
    {
        return 0; /* short, or a response */
    }
    char want[72], name[256];
    if (strlen(host) > 63)
    {
        return 0;
    }
    strcpy(want, host);
    strcat(want, ".local");
    for (char *c = want; *c; c++)
    {
        *c = (char)tolower(*c);
    }
    int qd = (in[4] << 8) | in[5];
    int pos = 12;
    for (int q = 0; q < qd; q++)
    {
        pos = read_name(in, in_len, pos, name, sizeof(name));
        if (pos < 0 || pos + 4 > in_len)
        {
            return 0;
        }
        uint16_t type = (uint16_t)((in[pos] << 8) | in[pos + 1]);
        pos += 4;
        if ((type == 1 || type == 255) && !strcmp(name, want))
        {
            return build(host, ip, legacy ? (uint16_t)((in[0] << 8) | in[1]) : 0, legacy, out, out_size);
        }
    }
    return 0;
}

int mdns_min_announce(const char *host, uint32_t ip, uint8_t *out, int out_size)
{
    return build(host, ip, 0, false, out, out_size);
}
