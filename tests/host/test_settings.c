/* settings_save_ini() followed by settings_load_ini() must give the same values */
#include <stdio.h>
#include <string.h>
#include "settings.h"

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
    s->ota_allow_overwrite_pd = true;
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
    CHECK(s->ota_allow_overwrite_pd);

    /* host set but cloud switched off must survive the round trip */
    s->cloud_enabled = false;
    CHECK(settings_save_ini(SETTINGS_INI_PATH) == 0);
    s->cloud_enabled = true;
    settings_load_ini();
    CHECK(!s->cloud_enabled);

    remove(SETTINGS_INI_PATH);
    printf(fails ? "settings: %d FAILED\n" : "settings: all tests ok\n", fails);
    return fails;
}
