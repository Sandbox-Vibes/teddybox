#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Settings are taken from two sources, later ones override earlier ones:
 *
 *  1. built-in defaults
 *  2. NVS namespace "TB_SETTINGS" written by the original Boxine firmware
 *     (LANG_ID, FAC_LANG_ID, REGION, BAT_CRITICAL, BAT_LOW_LEVEL_D/U)
 *  3. optional text file /sdcard/teddybox.ini with "key=value" lines
 *
 * See README.md for the list of keys.
 */

#ifndef SETTINGS_INI_PATH
#define SETTINGS_INI_PATH "/sdcard/teddybox.ini"
#endif

typedef struct
{
    /* language folder for system sounds (0=EN-GB, 1=DE, 2=EN-US, 3=FR) */
    uint8_t lang_id;
    char region[8];

    /* battery thresholds in mV, defaults taken from the original NVS */
    uint16_t bat_critical_mv;
    uint16_t bat_low_down_mv;
    uint16_t bat_low_up_mv;

    /* cloud / teddyCloud connection */
    bool cloud_enabled;
    char cloud_host[64];
    uint16_t cloud_port;
    char ca_path[64];

    /* Boxine cloud: used directly when teddyCloud is not configured or not
       reachable. Off by default, see README (box certificate could be blocked) */
    bool boxine_fallback;
    char boxine_host[48];
    /* send the original firmware's user agent ("toniebox-esp32-eu/v5.x")
       instead of "toniebox-esp32-teddybox/<version>"; teddyCloud passes it on */
    bool user_agent_original;

    /* volume limits in percent (0..100) */
    uint8_t volume_max;
    uint8_t volume_default;

    /* idle time until power off, seconds */
    uint32_t idle_timeout_s;

    /* MQTT / Home Assistant, off while mqtt_host is empty */
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[32];
    char mqtt_pass[64];
    /* base topic, default "teddybox/<mac>" */
    char mqtt_topic[48];
    char ha_discovery_prefix[32];

    } settings_t;

/* load NVS part, call after nvs_flash_init() */
void settings_init(void);
/* apply /sdcard/teddybox.ini, call after the SD card is mounted */
void settings_load_ini(void);

const settings_t *settings_get(void);

/* for the setup portal: modify a copy and write it with settings_save_ini() */
settings_t *settings_get_mutable(void);
/* writes all INI keys to path (SETTINGS_INI_PATH on the box), returns 0 on success */
int settings_save_ini(const char *path);
/* true if SETTINGS_INI_PATH exists */
int settings_ini_exists(void);
