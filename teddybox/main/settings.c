#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "speaker_eq.h"
#include "settings.h"

static const char *TAG = "Settings";

static settings_t settings = {
    .lang_id = 0,
    .region = "EU",
    .bat_critical_mv = 3600,
    .bat_low_down_mv = 3670,
    .bat_low_up_mv = 3700,
    /* off until a host is configured: talking to the Boxine cloud with a
       non-original firmware could get the box certificate blocked */
    .cloud_enabled = false,
    .cloud_host = "",
    .cloud_port = 443,
    .ca_path = "/spiflash/cert/ca.der",
    .boxine_fallback = false,
    .boxine_host = "prod.de.tbs.toys",
    .user_agent_original = false,
    .volume_max = 100,
    .volume_default = 50,
    .idle_timeout_s = 60,
    .speaker_eq = SPEAKER_EQ_DEFAULT,
    .led_brightness = 3,
    .slap_level = 3,
    .slap_reverse = false,
    .tilt_level = 3,
    .mem_log = true,
    .update_hour = 3,
    .claim = false,
    .decoder_celt = true,
    .cpu_mhz = 160,
    .dl_playing_kib_s = 96,
    .timezone = "CET-1CEST,M3.5.0,M10.5.0/3",
    .mqtt_host = "",
    .mqtt_port = 1883,
    .mqtt_user = "",
    .mqtt_pass = "",
    .mqtt_topic = "",
    .ha_discovery_prefix = "homeassistant",
};

const settings_t *settings_get(void)
{
    return &settings;
}

settings_t *settings_get_mutable(void)
{
    return &settings;
}

int settings_ini_exists(void)
{
    FILE *f = fopen(SETTINGS_INI_PATH, "r");
    if (f)
    {
        fclose(f);
    }
    return f != NULL;
}

int settings_save_ini(const char *path)
{
    char tmp[72];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
    {
        ESP_LOGE(TAG, "Cannot write %s", tmp);
        return -1;
    }

    const settings_t *s = &settings;
    int ok = fprintf(f,
                     "# TeddyBox settings, written by the setup page (hold the big ear while switching on)\n"
                     "\n"
                     "# teddyCloud, cloud is off while cloud_host is empty\n"
                     "cloud_host = %s\n"
                     "cloud_port = %u\n"
                     "ca_path = %s\n"
                     "%s"
                     "\n"
                     "# Boxine cloud directly when teddyCloud is missing or down (box certificate could be blocked)\n"
                     "boxine_fallback = %d\n"
                     "boxine_host = %s\n"
                     "# original firmware user agent instead of toniebox-esp32-teddybox\n"
                     "user_agent_original = %d\n"
                     "\n"
                     "# Home Assistant / MQTT, off while mqtt_host is empty\n"
                     "mqtt_host = %s\n"
                     "mqtt_port = %u\n"
                     "mqtt_user = %s\n"
                     "mqtt_pass = %s\n"
                     "%s%s%s"
                     "ha_discovery_prefix = %s\n"
                     "\n"
                     "# box\n"
                     "lang_id = %u\n"
                     "volume_max = %u\n"
                     "volume_default = %u\n"
                     "idle_timeout = %u\n"
                     "# speaker EQ: off, lowcut (high-pass 150 Hz) or full (plus gentle cuts at 400 Hz and 3.2 kHz)\n"
                     "speaker_eq = %s\n"
                     "# LED brightness: 0 (off) to 3 (full)\n"
                     "led_brightness = %u\n"
                     "# slap on the side for the next/previous chapter: 0 (off), 1 (firm) to 5 (light), 3 like the original\n"
                     "slap = %u\n"
                     "slap_reverse = %u\n"
                     "# tilt for chapter/seek: 0 (off), 1 (tilt far) to 5 (tilt a little), 3 = 30 degrees\n"
                     "tilt = %u\n"
                     "# memory lines in the serial log (diagnostics): 1 on, 0 off\n"
                     "mem_log = %u\n"
                     "# nightly update of the tonies (new versions, unfinished downloads): hour 0-23, -1 off\n"
                     "update_hour = %d\n"
                     "# local time (POSIX TZ), default Germany/Central Europe\n"
                     "timezone = %s\n"
                     "# report placed tonies to teddyCloud like the original box; off: teddyCloud forwards them to the Boxine cloud\n"
                     "# if its cloud access is on (\"Forward claim\"), and Home Assistant shows the tonie anyway\n"
                     "claim = %u\n"
                     "# Opus decoder: celt (default, libopus CELT only, like the original firmware) or adf (ESP-ADF, also SILK/hybrid files)\n"
                     "decoder = %s\n"
                     "# CPU clock in MHz: 160 (default, about 34 %% of one core for decoding) or 240; decoder = adf always uses 240\n"
                     "cpu_mhz = %u\n"
                     "# download speed while a tonie plays in KiB/s (playback needs about 12), 0 = unlimited (about 250);\n"
                     "# without playback (nightly update, idle box) always unlimited\n"
                     "download_playing = %u\n",
                     s->cloud_host, s->cloud_port, s->ca_path,
                     (s->cloud_host[0] && !s->cloud_enabled) ? "cloud_enabled = 0\n" : "",
                     s->boxine_fallback ? 1 : 0,
                     s->boxine_host[0] ? s->boxine_host : "prod.de.tbs.toys",
                     s->user_agent_original ? 1 : 0,
                     s->mqtt_host, s->mqtt_port, s->mqtt_user, s->mqtt_pass,
                     s->mqtt_topic[0] ? "mqtt_topic = " : "", s->mqtt_topic, s->mqtt_topic[0] ? "\n" : "",
                     s->ha_discovery_prefix,
                     s->lang_id, s->volume_max, s->volume_default, (unsigned)s->idle_timeout_s,
                     speaker_eq_name(s->speaker_eq), s->led_brightness,
                     s->slap_level, s->slap_reverse ? 1 : 0, s->tilt_level, s->mem_log ? 1 : 0, s->update_hour, s->timezone, s->claim ? 1 : 0, s->decoder_celt ? "celt" : "adf", s->cpu_mhz, s->dl_playing_kib_s) > 0;
    ok = (fclose(f) == 0) && ok;

    if (!ok)
    {
        unlink(tmp);
        return -1;
    }
    unlink(path);
    if (rename(tmp, path) != 0)
    {
        unlink(tmp);
        return -1;
    }
    ESP_LOGI(TAG, "Saved %s", path);
    return 0;
}

void settings_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    nvs_handle_t h;
    if (nvs_open("TB_SETTINGS", NVS_READONLY, &h) != ESP_OK)
    {
        ESP_LOGW(TAG, "No original TB_SETTINGS in NVS, using defaults");
        return;
    }

    uint8_t lang = 0, fac_lang = 0;
    bool have_lang = nvs_get_u8(h, "LANG_ID", &lang) == ESP_OK;
    bool have_fac = nvs_get_u8(h, "FAC_LANG_ID", &fac_lang) == ESP_OK;

    /* the original firmware keeps LANG_ID at 0 unless the user changed it,
       the factory language is the one the box was sold with */
    if (have_lang && lang != 0)
    {
        settings.lang_id = lang;
    }
    else if (have_fac)
    {
        settings.lang_id = fac_lang;
    }

    size_t len = sizeof(settings.region);
    nvs_get_str(h, "REGION", settings.region, &len);

    uint16_t v;
    if (nvs_get_u16(h, "BAT_CRITICAL", &v) == ESP_OK && v > 3000 && v < 4500)
    {
        settings.bat_critical_mv = v;
    }
    if (nvs_get_u16(h, "BAT_LOW_LEVEL_D", &v) == ESP_OK && v > 3000 && v < 4500)
    {
        settings.bat_low_down_mv = v;
    }
    if (nvs_get_u16(h, "BAT_LOW_LEVEL_U", &v) == ESP_OK && v > 3000 && v < 4500)
    {
        settings.bat_low_up_mv = v;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "NVS: lang %d, region %s, battery critical/low/ok %d/%d/%d mV",
             settings.lang_id, settings.region,
             settings.bat_critical_mv, settings.bat_low_down_mv, settings.bat_low_up_mv);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
    {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
    {
        *--end = 0;
    }
    return s;
}

static bool parse_bool(const char *v)
{
    return !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on");
}

static void copy_str(char *dst, size_t size, const char *src)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = 0;
}

void settings_load_ini(void)
{
    FILE *f = fopen(SETTINGS_INI_PATH, "r");
    if (!f)
    {
        ESP_LOGI(TAG, "No %s, keeping defaults", SETTINGS_INI_PATH);
        return;
    }

    char line[128];
    int cloud_enabled_explicit = -1;
    while (fgets(line, sizeof(line), f))
    {
        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';' || *l == '[')
        {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq)
        {
            continue;
        }
        *eq = 0;
        char *key = trim(l);
        char *val = trim(eq + 1);
        long num = strtol(val, NULL, 0);

        if (!strcmp(key, "cloud_enabled"))
        {
            cloud_enabled_explicit = parse_bool(val);
        }
        else if (!strcmp(key, "cloud_host"))
        {
            copy_str(settings.cloud_host, sizeof(settings.cloud_host), val);
        }
        else if (!strcmp(key, "cloud_port") && num > 0 && num < 65536)
        {
            settings.cloud_port = num;
        }
        else if (!strcmp(key, "ca_path"))
        {
            copy_str(settings.ca_path, sizeof(settings.ca_path), val);
        }
        else if (!strcmp(key, "lang_id") && num >= 0 && num < 256)
        {
            settings.lang_id = num;
        }
        else if (!strcmp(key, "volume_max") && num >= 10 && num <= 100)
        {
            settings.volume_max = num;
        }
        else if (!strcmp(key, "volume_default") && num >= 0 && num <= 100)
        {
            settings.volume_default = num;
        }
        else if (!strcmp(key, "led_brightness") && num >= 0 && num <= 3 && val[0] >= '0' && val[0] <= '3' && !val[1])
        {
            settings.led_brightness = num;
        }
        else if (!strcmp(key, "decoder"))
        {
            if (!strcmp(val, "adf") || !strcmp(val, "celt"))
            {
                settings.decoder_celt = !strcmp(val, "celt");
            }
            else
            {
                ESP_LOGW(TAG, "Invalid decoder '%s' (adf or celt)", val);
            }
        }
        else if (!strcmp(key, "cpu_mhz"))
        {
            if (!strcmp(val, "160") || !strcmp(val, "240"))
            {
                settings.cpu_mhz = atoi(val);
            }
            else
            {
                ESP_LOGW(TAG, "Invalid cpu_mhz '%s' (160 or 240)", val);
            }
        }
        else if (!strcmp(key, "download_playing"))
        {
            char *end;
            long v = strtol(val, &end, 10);
            if (*val && !*end && (v == 0 || (v >= 32 && v <= 4096)))
            {
                settings.dl_playing_kib_s = v;
            }
            else
            {
                ESP_LOGW(TAG, "Invalid download_playing '%s' (0 or 32..4096 KiB/s)", val);
            }
        }
        else if (!strcmp(key, "claim"))
        {
            if (!strcmp(val, "0") || !strcmp(val, "1"))
            {
                settings.claim = val[0] == '1';
            }
            else
            {
                ESP_LOGW(TAG, "Invalid claim '%s' (0 or 1)", val);
            }
        }
        else if (!strcmp(key, "update_hour"))
        {
            char *end;
            long h = strtol(val, &end, 10);
            if (*val && !*end && h >= -1 && h <= 23)
            {
                settings.update_hour = h;
            }
            else
            {
                ESP_LOGW(TAG, "Invalid update_hour '%s' (0 to 23, -1 off)", val);
            }
        }
        else if (!strcmp(key, "timezone"))
        {
            if (strlen(val) > 0 && strlen(val) < sizeof(settings.timezone) && !strchr(val, '\n'))
            {
                snprintf(settings.timezone, sizeof(settings.timezone), "%s", val);
            }
            else
            {
                ESP_LOGW(TAG, "Invalid timezone '%s'", val);
            }
        }
        else if (!strcmp(key, "mem_log"))
        {
            if ((val[0] == '0' || val[0] == '1') && !val[1])
            {
                settings.mem_log = num;
            }
            else
            {
                ESP_LOGW(TAG, "Invalid mem_log '%s' (0 or 1)", val);
            }
        }
        else if (!strcmp(key, "slap") || !strcmp(key, "tilt") || !strcmp(key, "slap_reverse"))
        {
            int max = strcmp(key, "slap_reverse") ? 5 : 1;
            if (val[0] >= '0' && val[0] <= '0' + max && !val[1])
            {
                if (!strcmp(key, "slap"))
                {
                    settings.slap_level = num;
                }
                else if (!strcmp(key, "tilt"))
                {
                    settings.tilt_level = num;
                }
                else
                {
                    settings.slap_reverse = num;
                }
            }
            else
            {
                ESP_LOGW(TAG, "Invalid %s '%s' (0 to %d)", key, val, max);
            }
        }
        else if (!strcmp(key, "speaker_eq"))
        {
            if (speaker_eq_parse(val) >= 0)
            {
                settings.speaker_eq = speaker_eq_parse(val);
            }
            else
            {
                ESP_LOGW(TAG, "Invalid speaker_eq '%s' (off, lowcut, full)", val);
            }
        }
        else if (!strcmp(key, "idle_timeout") && num >= 10)
        {
            settings.idle_timeout_s = num;
        }
        else if (!strcmp(key, "boxine_fallback"))
        {
            settings.boxine_fallback = parse_bool(val);
        }
        else if (!strcmp(key, "boxine_host"))
        {
            copy_str(settings.boxine_host, sizeof(settings.boxine_host), val);
        }
        else if (!strcmp(key, "user_agent_original"))
        {
            settings.user_agent_original = parse_bool(val);
        }
        else if (!strcmp(key, "mqtt_host"))
        {
            copy_str(settings.mqtt_host, sizeof(settings.mqtt_host), val);
        }
        else if (!strcmp(key, "mqtt_port") && num > 0 && num < 65536)
        {
            settings.mqtt_port = num;
        }
        else if (!strcmp(key, "mqtt_user"))
        {
            copy_str(settings.mqtt_user, sizeof(settings.mqtt_user), val);
        }
        else if (!strcmp(key, "mqtt_pass"))
        {
            copy_str(settings.mqtt_pass, sizeof(settings.mqtt_pass), val);
        }
        else if (!strcmp(key, "mqtt_topic"))
        {
            copy_str(settings.mqtt_topic, sizeof(settings.mqtt_topic), val);
        }
        else if (!strcmp(key, "ha_discovery_prefix"))
        {
            copy_str(settings.ha_discovery_prefix, sizeof(settings.ha_discovery_prefix), val);
        }
        else if (!strcmp(key, "ota_enabled") || !strcmp(key, "ota_allow_overwrite_pd"))
        {
            /* from older versions: updates now only go through the setup page */
        }
        else
        {
            ESP_LOGW(TAG, "Unknown key '%s'", key);
        }
    }
    fclose(f);

    /* a configured host enables the cloud unless cloud_enabled=0 is given */
    settings.cloud_enabled = settings.cloud_host[0] && cloud_enabled_explicit != 0;

    if (settings.volume_default > settings.volume_max)
    {
        settings.volume_default = settings.volume_max;
    }

    if (settings.mqtt_host[0])
    {
        ESP_LOGI(TAG, "INI: MQTT %s:%d%s", settings.mqtt_host, settings.mqtt_port, settings.mqtt_user[0] ? " with login" : "");
    }
    if (settings.boxine_fallback || settings.user_agent_original)
    {
        ESP_LOGW(TAG, "INI: Boxine fallback %s (%s), original user agent %s",
                 settings.boxine_fallback ? "ON" : "off", settings.boxine_host, settings.user_agent_original ? "ON" : "off");
    }
    ESP_LOGI(TAG, "INI: cloud %s %s:%d (CA %s), lang %d, volume max %d, idle %ds",
             settings.cloud_enabled ? "on" : "off", settings.cloud_host, settings.cloud_port,
             settings.ca_path, settings.lang_id, settings.volume_max, settings.idle_timeout_s);
}
