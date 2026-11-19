#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "mqtt_ha_payload.h"


typedef struct
{
    const char *component; /* sensor, binary_sensor, number, button */
    const char *object;    /* unique part of the entity id */
    const char *name;
    const char *extra;     /* additional JSON members, "%s" is replaced by the base topic */
} ha_entity_t;

static const ha_entity_t entities[] = {
    {"sensor", "status", "Status",
     "\"value_template\":\"{{ value_json.status }}\",\"icon\":\"mdi:play-pause\""},
    {"sensor", "tonie", "Tonie",
     "\"value_template\":\"{{ value_json.tonie if value_json.tonie else 'kein Tonie' }}\",\"icon\":\"mdi:nfc-variant\""},
    {"sensor", "chapter", "Kapitel",
     "\"value_template\":\"{{ value_json.chapter if value_json.chapters else 0 }}\",\"icon\":\"mdi:book-open-page-variant\""},
    {"sensor", "chapters", "Kapitel gesamt",
     "\"value_template\":\"{{ value_json.chapters }}\",\"icon\":\"mdi:book-multiple\",\"entity_category\":\"diagnostic\""},
    /* progress: null in the state JSON renders as "None", which HA shows as unknown */
    {"sensor", "progress", "Fortschritt",
     "\"value_template\":\"{{ value_json.progress }}\",\"unit_of_measurement\":\"%%\",\"icon\":\"mdi:progress-clock\""},
    {"sensor", "chapter_progress", "Kapitel-Fortschritt",
     "\"value_template\":\"{{ value_json.chapter_progress }}\",\"unit_of_measurement\":\"%%\",\"icon\":\"mdi:progress-helper\""},
    {"sensor", "position", "Gespielt",
     "\"value_template\":\"{{ value_json.position }}\",\"device_class\":\"duration\",\"unit_of_measurement\":\"min\","
     "\"suggested_display_precision\":0"},
    {"sensor", "duration", "Gesamtdauer",
     "\"value_template\":\"{{ value_json.duration }}\",\"device_class\":\"duration\",\"unit_of_measurement\":\"min\","
     "\"suggested_display_precision\":0"},
    {"sensor", "download", "Download",
     "\"value_template\":\"{{ value_json.download }}\",\"unit_of_measurement\":\"%%\",\"icon\":\"mdi:download\""},
    {"number", "volume", "Lautstärke",
     "\"value_template\":\"{{ value_json.volume }}\",\"command_topic\":\"%s/cmd/volume\",\"min\":0,\"max\":%d,\"step\":5,"
     "\"unit_of_measurement\":\"%%\",\"icon\":\"mdi:volume-high\""},
    {"sensor", "battery", "Akku (geschätzt)",
     "\"value_template\":\"{{ value_json.battery if value_json.battery >= 0 else None }}\",\"device_class\":\"battery\","
     "\"unit_of_measurement\":\"%%\",\"state_class\":\"measurement\""},
    {"sensor", "battery_voltage", "Akkuspannung",
     "\"value_template\":\"{{ (value_json.battery_mv / 1000) | round(2) if value_json.battery_mv else None }}\","
     "\"device_class\":\"voltage\",\"unit_of_measurement\":\"V\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\""},
    {"binary_sensor", "charging", "Lädt",
     "\"value_template\":\"{{ 'ON' if value_json.charging else 'OFF' }}\",\"device_class\":\"battery_charging\""},
    {"binary_sensor", "battery_low", "Akku schwach",
     "\"value_template\":\"{{ 'ON' if value_json.battery_state in ['low', 'critical'] else 'OFF' }}\",\"device_class\":\"battery\""},
    {"binary_sensor", "headphones", "Kopfhörer",
     "\"value_template\":\"{{ 'ON' if value_json.headphones else 'OFF' }}\",\"icon\":\"mdi:headphones\""},
    {"sensor", "rssi", "WLAN-Signal",
     "\"value_template\":\"{{ value_json.rssi if value_json.rssi else None }}\",\"device_class\":\"signal_strength\","
     "\"unit_of_measurement\":\"dBm\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\""},
    {"number", "led", "LED-Helligkeit",
     "\"value_template\":\"{{ value_json.led }}\",\"command_topic\":\"%s/cmd/led\",\"min\":0,\"max\":3,\"step\":1,"
     "\"mode\":\"slider\",\"icon\":\"mdi:led-on\",\"entity_category\":\"config\""},
    {"select", "speaker_eq", "Lautsprecher-Klang",
     "\"value_template\":\"{{ value_json.speaker_eq }}\",\"command_topic\":\"%s/cmd/speaker_eq\","
     "\"options\":[\"Optimiert\",\"Nur Tiefenfilter\",\"Wie Original\"],\"icon\":\"mdi:tune-vertical\",\"entity_category\":\"config\""},
    {"button", "stop", "Stopp",
     "\"command_topic\":\"%s/cmd/stop\",\"icon\":\"mdi:stop\""},
    {"button", "next", "Nächstes Kapitel",
     "\"command_topic\":\"%s/cmd/next\",\"icon\":\"mdi:skip-next\""},
    {"button", "prev", "Vorheriges Kapitel",
     "\"command_topic\":\"%s/cmd/prev\",\"icon\":\"mdi:skip-previous\""},
    {"button", "poweroff", "Ausschalten",
     "\"command_topic\":\"%s/cmd/poweroff\",\"icon\":\"mdi:power\""},
};

#define ENTITY_COUNT ((int)(sizeof(entities) / sizeof(entities[0])))

static const char *const speaker_eq_labels[HA_SPEAKER_EQ_COUNT] = {"Wie Original", "Nur Tiefenfilter", "Optimiert"};

const char *ha_speaker_eq_label(int mode)
{
    return (mode >= 0 && mode < HA_SPEAKER_EQ_COUNT) ? speaker_eq_labels[mode] : speaker_eq_labels[0];
}

int ha_discovery_count(void)
{
    return ENTITY_COUNT;
}

/* formats e->extra: replaces the first %s with the base topic and %d with volume_max */
static int format_extra(const ha_ctx_t *ctx, const ha_entity_t *e, char *buf, size_t size)
{
    int n;
    if (!strcmp(e->object, "volume"))
    {
        n = snprintf(buf, size, e->extra, ctx->base_topic, ctx->volume_max);
    }
    else if (strstr(e->extra, "%s"))
    {
        n = snprintf(buf, size, e->extra, ctx->base_topic);
    }
    else
    {
        n = snprintf(buf, size, e->extra, 0); /* only "%%" escapes */
    }
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

int ha_discovery(const ha_ctx_t *ctx, int idx, char *topic, size_t topic_size, char *payload, size_t payload_size)
{
    if (idx < 0 || idx >= ENTITY_COUNT)
    {
        return -1;
    }
    const ha_entity_t *e = &entities[idx];

    int n = snprintf(topic, topic_size, "%s/%s/%s/%s/config", ctx->prefix, e->component, ctx->device_id, e->object);
    if (n < 0 || (size_t)n >= topic_size)
    {
        return -1;
    }

    char extra[400];
    if (format_extra(ctx, e, extra, sizeof(extra)) < 0)
    {
        return -1;
    }

    /* buttons have no state */
    char state_topic[96] = "";
    if (strcmp(e->component, "button"))
    {
        snprintf(state_topic, sizeof(state_topic), "\"state_topic\":\"%s/state\",", ctx->base_topic);
    }

    /* default_entity_id replaces object_id, which Home Assistant dropped in 2026.4 */
    n = snprintf(payload, payload_size,
                 "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"default_entity_id\":\"%s.%s_%s\",%s"
                 "\"availability_topic\":\"%s/availability\",%s,"
                 "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"TeddyBox\",\"manufacturer\":\"toniebox-reverse-engineering\","
                 "\"model\":\"Toniebox ESP32 (TeddyBox)\",\"sw_version\":\"%s\"},"
                 "\"origin\":{\"name\":\"TeddyBox\",\"sw_version\":\"%s\",\"url\":\"https://github.com/toniebox-reverse-engineering/teddybox\"}}",
                 e->name, ctx->device_id, e->object, e->component, ctx->device_id, e->object, state_topic,
                 ctx->base_topic, extra,
                 ctx->device_id, ctx->sw_version, ctx->sw_version);
    return (n < 0 || (size_t)n >= payload_size) ? -1 : n;
}

/* -1 = unknown -> null */
static const char *ha_int_or_null(char *buf, size_t size, int v)
{
    if (v < 0)
    {
        return "null";
    }
    snprintf(buf, size, "%d", v);
    return buf;
}

/* tenths of a second -> minutes with one decimal, -1 -> null */
static const char *ha_minutes_or_null(char *buf, size_t size, int ds)
{
    if (ds < 0)
    {
        return "null";
    }
    snprintf(buf, size, "%d.%d", ds / 600, (ds % 600) / 60);
    return buf;
}

int ha_state_json(const ha_state_t *st, char *buf, size_t size)
{
    char prog[8], chprog[8], pos[16], dur[16], dl[8];
    char tonie[20] = "";
    if (st->uid)
    {
        snprintf(tonie, sizeof(tonie), "%016" PRIX64, st->uid);
    }

    int n = snprintf(buf, size,
                     "{\"status\":\"%s\",\"tonie\":\"%s\",\"chapter\":%d,\"chapters\":%d,\"volume\":%u,"
                     "\"battery_mv\":%u,\"battery\":%d,\"battery_state\":\"%s\",\"charging\":%s,"
                     "\"headphones\":%s,\"rssi\":%d,\"speaker_eq\":\"%s\",\"led\":%d,"
                     "\"progress\":%s,\"chapter_progress\":%s,\"position\":%s,\"duration\":%s,\"download\":%s}",
                     st->playing ? "playing" : "idle", tonie, st->chapter, st->chapters, st->volume,
                     st->battery_mv, st->battery_percent, st->battery_state ? st->battery_state : "unknown",
                     st->charging ? "true" : "false", st->headphones ? "true" : "false", st->rssi,
                     ha_speaker_eq_label(st->speaker_eq), st->led,
                     ha_int_or_null(prog, sizeof(prog), st->progress), ha_int_or_null(chprog, sizeof(chprog), st->chapter_progress),
                     ha_minutes_or_null(pos, sizeof(pos), st->position_ds), ha_minutes_or_null(dur, sizeof(dur), st->duration_ds),
                     ha_int_or_null(dl, sizeof(dl), st->download));
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

ha_cmd_t ha_parse_command(const ha_ctx_t *ctx, const char *topic, int topic_len, const char *data, int data_len, int *value)
{
    size_t base_len = strlen(ctx->base_topic);
    static const char cmd[] = "/cmd/";

    if (topic_len <= (int)(base_len + sizeof(cmd) - 1) || strncmp(topic, ctx->base_topic, base_len) ||
        strncmp(topic + base_len, cmd, sizeof(cmd) - 1))
    {
        return HA_CMD_NONE;
    }
    const char *name = topic + base_len + sizeof(cmd) - 1;
    int name_len = topic_len - (int)(name - topic);

#define IS(s) (name_len == (int)strlen(s) && !strncmp(name, s, name_len))
    if (IS("volume") || IS("led"))
    {
        char num[12];
        if (data_len <= 0 || data_len >= (int)sizeof(num))
        {
            return HA_CMD_NONE;
        }
        memcpy(num, data, data_len);
        num[data_len] = 0;
        char *end;
        /* Home Assistant may send "50.0" */
        double v = strtod(num, &end);
        if (end == num)
        {
            return HA_CMD_NONE;
        }
        /* round half away from zero, (int)(v + 0.5) would turn -1 into 0 */
        *value = (int)(v + (v >= 0 ? 0.5 : -0.5));
        if (IS("led"))
        {
            return (*value >= 0 && *value <= 3) ? HA_CMD_LED : HA_CMD_NONE;
        }
        return HA_CMD_VOLUME;
    }
    if (IS("stop"))
    {
        return HA_CMD_STOP;
    }
    if (IS("next"))
    {
        return HA_CMD_NEXT;
    }
    if (IS("prev"))
    {
        return HA_CMD_PREV;
    }
    if (IS("poweroff"))
    {
        return HA_CMD_POWEROFF;
    }
    if (IS("speaker_eq"))
    {
        for (int i = 0; i < HA_SPEAKER_EQ_COUNT; i++)
        {
            if (data_len == (int)strlen(speaker_eq_labels[i]) && !strncmp(data, speaker_eq_labels[i], data_len))
            {
                *value = i;
                return HA_CMD_SPEAKER_EQ;
            }
        }
        return HA_CMD_NONE;
    }
#undef IS
    return HA_CMD_NONE;
}

/* State of charge of the 3 cell NiMH pack (50AA5S, 3.6 V) from the voltage
   at light load. NiMH stays nearly flat around 1.23-1.27 V per cell for most
   of its capacity, rises steeply when full and falls steeply when empty, so a
   straight line shows "almost empty" far too early. The points are mV above
   the critical threshold from the original firmware's NVS (default 3600 mV,
   where the box switches off = 0 %); 3670 mV ("battery low" in the original
   firmware) gives 10 %. An estimate from typical NiMH curves, not measured
   on this box. */
static const struct
{
    uint16_t above_critical_mv;
    uint8_t percent;
} nimh_curve[] = {
    {0, 0}, {70, 10}, {100, 20}, {130, 30}, {160, 45}, {200, 60}, {250, 75}, {300, 85}, {400, 95}, {550, 100},
};

int ha_battery_percent(unsigned mv, unsigned critical_mv)
{
    const int n = sizeof(nimh_curve) / sizeof(nimh_curve[0]);
    if (mv == 0)
    {
        return -1;
    }
    if (mv <= critical_mv)
    {
        return 0;
    }
    unsigned d = mv - critical_mv;
    for (int i = 1; i < n; i++)
    {
        if (d < nimh_curve[i].above_critical_mv)
        {
            unsigned d0 = nimh_curve[i - 1].above_critical_mv, d1 = nimh_curve[i].above_critical_mv;
            unsigned p0 = nimh_curve[i - 1].percent, p1 = nimh_curve[i].percent;
            return (int)(p0 + ((d - d0) * (p1 - p0) + (d1 - d0) / 2) / (d1 - d0));
        }
    }
    return 100;
}
