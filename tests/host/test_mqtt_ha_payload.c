#include <stdio.h>
#include <string.h>
#include "mqtt_ha_payload.h"
int main(int argc, char **argv)
{
    ha_ctx_t ctx = {"teddybox_aabbccddeeff", "teddybox/aabbccddeeff", "homeassistant", "testver", 80};
    char topic[128], payload[1024];
    if (argc > 1 && !strcmp(argv[1], "discovery")) {
        for (int i = 0; i < ha_discovery_count(); i++) {
            int n = ha_discovery(&ctx, i, topic, sizeof topic, payload, sizeof payload);
            if (n < 0) { fprintf(stderr, "FAIL discovery %d\n", i); return 1; }
            printf("%s\t%s\n", topic, payload);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "state")) {
        ha_state_t a = {.playing = 1, .uid = 0xE004035000112233ULL, .chapter = 3, .chapters = 12, .volume = 60,
                        .battery_mv = 3950, .battery_percent = ha_battery_percent(3950, 3600), .battery_state = "ok",
                        .charging = 0, .headphones = 1, .rssi = -61, .led = 2,
                        .progress = 37, .chapter_progress = 80, .position_ds = 7385, .duration_ds = 19980, .download = -1};
        ha_state_t b = {.battery_state = "unknown", .battery_percent = -1, .progress = -1, .chapter_progress = -1,
                        .position_ds = -1, .duration_ds = -1, .download = 55};
        char buf[512];
        ha_state_json(&a, buf, sizeof buf); printf("%s\n", buf);
        ha_state_json(&b, buf, sizeof buf); printf("%s\n", buf);
        return 0;
    }
    /* command parsing */
    struct { const char *t, *d; ha_cmd_t want; int val; } c[] = {
        {"teddybox/aabbccddeeff/cmd/volume", "50.0", HA_CMD_VOLUME, 50},
        {"teddybox/aabbccddeeff/cmd/volume", "35", HA_CMD_VOLUME, 35},
        {"teddybox/aabbccddeeff/cmd/volume", "abc", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/stop", "PRESS", HA_CMD_STOP, 0},
        {"teddybox/aabbccddeeff/cmd/next", "PRESS", HA_CMD_NEXT, 0},
        {"teddybox/aabbccddeeff/cmd/prev", "PRESS", HA_CMD_PREV, 0},
        {"teddybox/aabbccddeeff/cmd/poweroff", "PRESS", HA_CMD_POWEROFF, 0},
        {"teddybox/aabbccddeeff/cmd/stopx", "PRESS", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Optimiert", HA_CMD_SPEAKER_EQ, 2},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Nur Tiefenfilter", HA_CMD_SPEAKER_EQ, 1},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Wie Original", HA_CMD_SPEAKER_EQ, 0},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Optimier", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/led", "0", HA_CMD_LED, 0},
        {"teddybox/aabbccddeeff/cmd/led", "3.0", HA_CMD_LED, 3},
        {"teddybox/aabbccddeeff/cmd/led", "4", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/led", "-1", HA_CMD_NONE, 0},
        {"teddybox/other/cmd/stop", "PRESS", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/", "", HA_CMD_NONE, 0},
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        int v = 0;
        ha_cmd_t got = ha_parse_command(&ctx, c[i].t, strlen(c[i].t), c[i].d, strlen(c[i].d), &v);
        int ok = got == c[i].want && ((got != HA_CMD_VOLUME && got != HA_CMD_SPEAKER_EQ && got != HA_CMD_LED) || v == c[i].val);
        fails += !ok;
        printf("%s %-40s %-6s -> %d %d\n", ok ? "ok  " : "FAIL", c[i].t, c[i].d, got, v);
    }
    printf("battery%%: 3600->%d 3925->%d 4250->%d 4400->%d 0->%d\n", ha_battery_percent(3600,3600), ha_battery_percent(3925,3600),
           ha_battery_percent(4250,3600), ha_battery_percent(4400,3600), ha_battery_percent(0,3600));
    return fails;
}
