/* settings_save_ini() followed by settings_load_ini() must give the same values */
#include <stdio.h>
#include <string.h>
#include "settings.h"
#include "speaker_eq.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    remove(SETTINGS_INI_PATH);
    CHECK(!settings_ini_exists());
    settings_init();

    settings_t *s = settings_get_mutable();
    strcpy(s->cloud_host, "192.168.178.20");
    s->cloud_port = 8443;
    s->cloud_enabled = true;   /* the setup page sets this together with the host */
    strcpy(s->ca_path, "/sdcard/teddybox/ca.der");
    strcpy(s->mqtt_host, "ha.local");
    strcpy(s->mqtt_user, "teddy");
    strcpy(s->mqtt_pass, "p#ss=wo;rd");   /* characters that matter for INI parsing */
    s->lang_id = 3;
    s->volume_max = 70;
    s->idle_timeout_s = 300;
    s->speaker_eq = SPEAKER_EQ_LOWCUT;
    s->led_brightness = 1;
    s->slap_level = 5;
    s->slap_reverse = true;
    s->tilt_level = 0;
    s->boxine_fallback = true;
    s->user_agent_original = true;
    CHECK(settings_save_ini(SETTINGS_INI_PATH) == 0);
    CHECK(settings_ini_exists());

    /* reset to defaults by clobbering, then load */
    settings_t saved = *s;
    memset(s, 0, sizeof(*s));
    settings_init();
    settings_load_ini();
    CHECK(!strcmp(s->cloud_host, saved.cloud_host));
    CHECK(s->cloud_port == 8443);
    CHECK(s->cloud_enabled);
    CHECK(!strcmp(s->ca_path, saved.ca_path));
    CHECK(!strcmp(s->mqtt_host, "ha.local"));
    CHECK(!strcmp(s->mqtt_pass, "p#ss=wo;rd"));
    CHECK(s->lang_id == 3 && s->volume_max == 70 && s->idle_timeout_s == 300);
    CHECK(s->boxine_fallback && s->user_agent_original);
    CHECK(s->speaker_eq == SPEAKER_EQ_LOWCUT);
    CHECK(s->led_brightness == 1);
    CHECK(s->slap_level == 5 && s->slap_reverse && s->tilt_level == 0);
    CHECK(!strcmp(s->boxine_host, "prod.de.tbs.toys"));

    /* host set but cloud switched off must survive the round trip */
    s->cloud_enabled = false;
    CHECK(settings_save_ini(SETTINGS_INI_PATH) == 0);
    s->cloud_enabled = true;
    settings_load_ini();
    CHECK(!s->cloud_enabled);

    /* both switches are off by default */
    remove(SETTINGS_INI_PATH);
    memset(s, 0, sizeof(*s));
    settings_init();
    FILE *f = fopen(SETTINGS_INI_PATH, "w"); fputs("cloud_host = tc.local\n", f); fclose(f);
    s->boxine_fallback = false; s->user_agent_original = false; s->speaker_eq = SPEAKER_EQ_DEFAULT;
    settings_load_ini();
    CHECK(!s->boxine_fallback && !s->user_agent_original);
    CHECK(s->speaker_eq == SPEAKER_EQ_DEFAULT);
    f = fopen(SETTINGS_INI_PATH, "w"); fputs("speaker_eq = off\n", f); fclose(f);
    settings_load_ini();
    CHECK(s->speaker_eq == SPEAKER_EQ_OFF);
    f = fopen(SETTINGS_INI_PATH, "w"); fputs("speaker_eq = loud\n", f); fclose(f);
    settings_load_ini();
    CHECK(s->speaker_eq == SPEAKER_EQ_OFF); /* unknown value keeps the previous one */
    f = fopen(SETTINGS_INI_PATH, "w"); fputs("led_brightness = 0\n", f); fclose(f);
    settings_load_ini();
    CHECK(s->led_brightness == 0);
    f = fopen(SETTINGS_INI_PATH, "w"); fputs("led_brightness = 7\n", f); fclose(f);
    settings_load_ini();
    CHECK(s->led_brightness == 0);

    /* slap/tilt: defaults 3, invalid values keep the previous one */
    s->slap_level = 3; s->slap_reverse = false; s->tilt_level = 3;
    f = fopen(SETTINGS_INI_PATH, "w"); fputs("slap = 6\ntilt = 12\nslap_reverse = 2\n", f); fclose(f);
    settings_load_ini();
    CHECK(s->slap_level == 3 && !s->slap_reverse && s->tilt_level == 3);
    f = fopen(SETTINGS_INI_PATH, "w"); fputs("slap = 0\ntilt = 1\nslap_reverse = 1\n", f); fclose(f);
    settings_load_ini();
    CHECK(s->slap_level == 0 && s->slap_reverse && s->tilt_level == 1);

    /* nightly update: hour and timezone, invalid values keep the previous one */
    s->update_hour = 3; snprintf(s->timezone, sizeof(s->timezone), "CET-1CEST,M3.5.0,M10.5.0/3");
    f = fopen(SETTINGS_INI_PATH, "w"); fputs("update_hour = 24\ntimezone = \n", f); fclose(f);
    settings_load_ini();
    CHECK(s->update_hour == 3 && !strcmp(s->timezone, "CET-1CEST,M3.5.0,M10.5.0/3"));
    f = fopen(SETTINGS_INI_PATH, "w"); fputs("update_hour = -1\ntimezone = UTC0\n", f); fclose(f);
    settings_load_ini();
    CHECK(s->update_hour == -1 && !strcmp(s->timezone, "UTC0"));
    CHECK(settings_save_ini(SETTINGS_INI_PATH) == 0);
    s->update_hour = 5;
    settings_load_ini();
    CHECK(s->update_hour == -1 && !strcmp(s->timezone, "UTC0"));
    s->update_hour = 3; snprintf(s->timezone, sizeof(s->timezone), "CET-1CEST,M3.5.0,M10.5.0/3");

    /* keys of older versions are accepted silently */
    f = fopen(SETTINGS_INI_PATH, "w"); fputs("ota_enabled = 1\nota_allow_overwrite_pd = 1\n", f); fclose(f);
    settings_load_ini();

    remove(SETTINGS_INI_PATH);
    printf(fails ? "settings: %d FAILED\n" : "settings: all tests ok\n", fails);
    return fails;
}
