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
                        .charging = 0, .headphones = 1, .rssi = -61, .led = 2, .slap = 3, .tilt = 0, .slap_reverse = 1,
                        .progress = 37, .chapter_progress = 80, .position_ds = 7385, .duration_ds = 19980, .download = -1,
                        .sleep_timer = 25, .tonie_name = "Bibi \"Blocksberg\" – Hexen"};
        ha_state_t b = {.battery_state = "unknown", .battery_percent = -1, .progress = -1, .chapter_progress = -1,
                        .position_ds = -1, .duration_ds = -1, .download = 55};
        char buf[512];
        ha_state_json(&a, buf, sizeof buf); printf("%s\n", buf);
        ha_state_json(&b, buf, sizeof buf); printf("%s\n", buf);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "lang")) {
        /* English by default, German with ha_set_german(true) */
        int fails = 0, tonie = -1, eq = -1;
        for (int i = 0; i < ha_discovery_count(); i++) {
            ha_discovery(&ctx, i, topic, sizeof topic, payload, sizeof payload);
            if (strstr(topic, "/tonie_name/")) tonie = i;
            if (strstr(topic, "/speaker_eq/")) eq = i;
        }
        for (int de = 0; de < 2; de++) {
            ha_set_german(de);
            ha_discovery(&ctx, tonie, topic, sizeof topic, payload, sizeof payload);
            int ok = de ? strstr(payload, "\"name\":\"Tonie-Name\"") && strstr(payload, "'kein Tonie'")
                        : strstr(payload, "\"name\":\"Tonie name\"") && strstr(payload, "'no tonie'") && !strstr(payload, "kein");
            printf("%s %s names and templates\n", ok ? "ok  " : "FAIL", de ? "German" : "English"); fails += !ok;
            ha_discovery(&ctx, eq, topic, sizeof topic, payload, sizeof payload);
            ok = strstr(topic, "speaker_eq") && (de ? strstr(payload, "[\"Optimiert\",\"Nur Tiefenfilter\",\"Wie Original\"]")
                                                 : strstr(payload, "[\"Optimized\",\"Bass cut only\",\"Like original\"]"));
            printf("%s %s select options\n", ok ? "ok  " : "FAIL", de ? "German" : "English"); fails += !ok;
            ok = !strcmp(ha_speaker_eq_label(2), de ? "Optimiert" : "Optimized");
            printf("%s %s state label\n", ok ? "ok  " : "FAIL", de ? "German" : "English"); fails += !ok;
            for (int i = 0; i < ha_discovery_count(); i++)
                if (ha_discovery(&ctx, i, topic, sizeof topic, payload, sizeof payload) < 0) { printf("FAIL discovery %d\n", i); fails++; }
        }
        ha_set_german(0);
        /* link to the setup page on the device page */
        ha_discovery(&ctx, 0, topic, sizeof topic, payload, sizeof payload);
        int ok = !strstr(payload, "configuration_url");
        printf("%s no configuration_url without an address\n", ok ? "ok  " : "FAIL"); fails += !ok;
        ctx.config_url = "http://192.168.10.42/";
        ha_discovery(&ctx, 0, topic, sizeof topic, payload, sizeof payload);
        ok = strstr(payload, "\"sw_version\":\"testver\",\"configuration_url\":\"http://192.168.10.42/\"}") != NULL;
        printf("%s configuration_url in the device\n", ok ? "ok  " : "FAIL"); fails += !ok;
        ctx.config_url = NULL;
        return fails != 0;
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
        {"teddybox/aabbccddeeff/cmd/original", "PRESS", HA_CMD_BOOT_ORIGINAL, 0},
        {"teddybox/aabbccddeeff/cmd/stopx", "PRESS", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Optimiert", HA_CMD_SPEAKER_EQ, 2},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Nur Tiefenfilter", HA_CMD_SPEAKER_EQ, 1},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Wie Original", HA_CMD_SPEAKER_EQ, 0},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Optimier", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/setup", "PRESS", HA_CMD_SETUP, 0},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Optimized", HA_CMD_SPEAKER_EQ, 2},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Bass cut only", HA_CMD_SPEAKER_EQ, 1},
        {"teddybox/aabbccddeeff/cmd/speaker_eq", "Like original", HA_CMD_SPEAKER_EQ, 0},
        {"teddybox/aabbccddeeff/cmd/led", "0", HA_CMD_LED, 0},
        {"teddybox/aabbccddeeff/cmd/led", "3.0", HA_CMD_LED, 3},
        {"teddybox/aabbccddeeff/cmd/led", "4", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/led", "-1", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/slap", "0", HA_CMD_SLAP, 0},
        {"teddybox/aabbccddeeff/cmd/slap", "5.0", HA_CMD_SLAP, 5},
        {"teddybox/aabbccddeeff/cmd/slap", "6", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/tilt", "3", HA_CMD_TILT, 3},
        {"teddybox/aabbccddeeff/cmd/tilt", "-1", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/slap_reverse", "ON", HA_CMD_SLAP_REVERSE, 1},
        {"teddybox/aabbccddeeff/cmd/slap_reverse", "OFF", HA_CMD_SLAP_REVERSE, 0},
        {"teddybox/aabbccddeeff/cmd/slap_reverse", "on", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/sleep_timer", "30.0", HA_CMD_SLEEP_TIMER, 30},
        {"teddybox/aabbccddeeff/cmd/sleep_timer", "0", HA_CMD_SLEEP_TIMER, 0},
        {"teddybox/aabbccddeeff/cmd/sleep_timer", "241", HA_CMD_NONE, 0},
        {"teddybox/other/cmd/stop", "PRESS", HA_CMD_NONE, 0},
        {"teddybox/aabbccddeeff/cmd/", "", HA_CMD_NONE, 0},
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        int v = 0;
        ha_cmd_t got = ha_parse_command(&ctx, c[i].t, strlen(c[i].t), c[i].d, strlen(c[i].d), &v);
        int ok = got == c[i].want && ((got != HA_CMD_VOLUME && got != HA_CMD_SPEAKER_EQ && got != HA_CMD_LED && got != HA_CMD_SLEEP_TIMER) || v == c[i].val);
        fails += !ok;
        printf("%s %-40s %-6s -> %d %d\n", ok ? "ok  " : "FAIL", c[i].t, c[i].d, got, v);
    }
    /* NiMH curve: thresholds, monotonic, interpolation, shift with a different critical value */
    struct { unsigned mv, crit; int want; } b[] = {
        {0, 3600, -1}, {3500, 3600, 0}, {3600, 3600, 0}, {3670, 3600, 10}, {3700, 3600, 20},
        {3747, 3600, 39}, {3800, 3600, 60}, {3850, 3600, 75}, {4000, 3600, 95}, {4150, 3600, 100},
        {4634, 3600, 100}, {3720, 3650, 10},
    };
    for (unsigned i = 0; i < sizeof b / sizeof b[0]; i++) {
        int got = ha_battery_percent(b[i].mv, b[i].crit);
        int ok = got == b[i].want;
        fails += !ok;
        printf("%s battery %u mV (critical %u) -> %d %%\n", ok ? "ok  " : "FAIL", b[i].mv, b[i].crit, got);
    }
    for (unsigned mv = 3600, last = 0; mv <= 4200; mv += 5) {
        int p = ha_battery_percent(mv, 3600);
        if (p < (int)last) { fails++; printf("FAIL battery curve not monotonic at %u\n", mv); }
        last = p;
    }
    /* diagnostics */
    {
        char d[192];
        ha_diag_t dg = {.free_kb = 251, .min_free_kb = 138, .boot_iso = "2026-11-20T11:05:00+00:00", .reset = "task watchdog", .slot = "ota_1", .update = "21.11. 03:12: 2 Tonie(s) aktualisiert"};
        int n = ha_diag_json(&dg, d, sizeof(d));
        int ok = n > 0 && !strcmp(d, "{\"free_kb\":251,\"min_free_kb\":138,\"boot\":\"2026-11-20T11:05:00+00:00\",\"reset\":\"task watchdog\",\"slot\":\"ota_1\",\"update\":\"21.11. 03:12: 2 Tonie(s) aktualisiert\"}");
        fails += !ok;
        printf("%s diag %s\n", ok ? "ok  " : "FAIL", d);
        dg.boot_iso = "";
        n = ha_diag_json(&dg, d, sizeof(d));
        ok = n > 0 && strstr(d, "\"boot\":null");
        fails += !ok;
        printf("%s diag without clock: boot null\n", ok ? "ok  " : "FAIL");
    }
    return fails;
}
