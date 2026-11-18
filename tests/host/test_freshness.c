/* unit tests for the pure helpers in freshness.c (file and cloud parts are stubbed out) */
#include <stdio.h>
#include <string.h>
#include "freshness.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    uint64_t uid;
    CHECK(freshness_uid_from_names("33221100", "500304E0", &uid) && uid == 0xE004035000112233ULL);
    CHECK(freshness_uid_from_names("aabbccdd", "500304e0", &uid) && uid == 0xE0040350DDCCBBAAULL);
    CHECK(!freshness_uid_from_names("00000001", "00000000", &uid));   /* system sound */
    CHECK(!freshness_uid_from_names("33221100", "500304E0.old", &uid));
    CHECK(!freshness_uid_from_names("3322110", "500304E0", &uid));

    /* field 1 fixed64 twice (unpacked), then fields 2..8 as varints, as teddyCloud sends it */
    const uint8_t unpacked[] = {
        0x09, 0x33, 0x22, 0x11, 0x00, 0x50, 0x03, 0x04, 0xE0,
        0x09, 0x77, 0x66, 0x55, 0x44, 0x50, 0x03, 0x04, 0xE0,
        0x10, 0x00, 0x18, 0x02, 0x20, 0x01, 0x28, 0x00, 0x30, 0x00, 0x38, 0x03, 0x40, 0x02};
    uint64_t m[4];
    freshness_settings_t s;
    int n = freshness_parse_response(unpacked, sizeof unpacked, m, 4, &s);
    CHECK(n == 2 && m[0] == 0xE004035000112233ULL && m[1] == 0xE004035044556677ULL);
    CHECK(s.max_vol_spk == 2 && s.slap_en == 1 && s.slap_dir == 0 && s.max_vol_hdp == 3 && s.led == 2);

    /* packed variant and a missing field */
    const uint8_t packed[] = {0x0A, 0x08, 0x33, 0x22, 0x11, 0x00, 0x50, 0x03, 0x04, 0xE0, 0x18, 0x01};
    n = freshness_parse_response(packed, sizeof packed, m, 4, &s);
    CHECK(n == 1 && m[0] == 0xE004035000112233ULL && s.max_vol_spk == 1 && s.led == -1);

    /* empty response: nothing marked */
    CHECK(freshness_parse_response(packed, 0, m, 4, &s) == 0);
    /* truncated */
    CHECK(freshness_parse_response(unpacked, 5, m, 4, &s) == -1);
    /* more marked than space: only max stored */
    CHECK(freshness_parse_response(unpacked, sizeof unpacked, m, 1, &s) == 1);

    printf(fails ? "freshness: %d FAILED\n" : "freshness: all tests ok\n", fails);
    return fails;
}
