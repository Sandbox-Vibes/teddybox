#pragma once

/*
 * Home Assistant MQTT discovery and state payloads. Plain C without ESP-IDF
 * dependencies so it can be unit tested on a PC.
 *
 * Topics (BASE = mqtt_topic, default "teddybox/<mac>"):
 *   BASE/availability       "online" / "offline" (retained, LWT)
 *   BASE/state              JSON with all values (retained)
 *   BASE/cmd/volume         0..volume_max
 *   BASE/cmd/speaker_eq     one of the HA_SPEAKER_EQ_OPTIONS labels
 *   BASE/cmd/stop|next|prev|poweroff   any payload
 *   <prefix>/<component>/<device id>/<object>/config   discovery (retained)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    const char *device_id;   /* "teddybox_aabbccddeeff" */
    const char *base_topic;  /* "teddybox/aabbccddeeff" */
    const char *prefix;      /* "homeassistant" */
    const char *sw_version;
    int volume_max;
} ha_ctx_t;

typedef struct
{
    bool playing;
    uint64_t uid;            /* 0 = no tonie */
    int chapter;             /* 1-based for display, 0 = none */
    int chapters;
    unsigned volume;
    unsigned battery_mv;     /* 0 = unknown */
    int battery_percent;     /* -1 = unknown */
    const char *battery_state;
    bool charging;
    bool headphones;
    int rssi;                /* 0 = unknown */
    int speaker_eq;          /* 0 off, 1 lowcut, 2 full (speaker_eq_t) */
} ha_state_t;

typedef enum
{
    HA_CMD_NONE = 0,
    HA_CMD_VOLUME,
    HA_CMD_STOP,
    HA_CMD_NEXT,
    HA_CMD_PREV,
    HA_CMD_POWEROFF,
    HA_CMD_SPEAKER_EQ,
} ha_cmd_t;

/* select labels, index = speaker_eq_t */
#define HA_SPEAKER_EQ_COUNT 3
const char *ha_speaker_eq_label(int mode);

/* number of discovery messages */
int ha_discovery_count(void);

/* builds discovery message idx; returns payload length or -1 on error/overflow */
int ha_discovery(const ha_ctx_t *ctx, int idx, char *topic, size_t topic_size, char *payload, size_t payload_size);

/* state JSON for BASE/state; returns length or -1 */
int ha_state_json(const ha_state_t *st, char *buf, size_t size);

/* maps a received topic (full) and payload to a command; *value gets the volume or EQ mode */
ha_cmd_t ha_parse_command(const ha_ctx_t *ctx, const char *topic, int topic_len, const char *data, int data_len, int *value);

/* rough charge estimate for the 3 cell NiMH pack, linear between critical and full */
int ha_battery_percent(unsigned mv, unsigned critical_mv);
