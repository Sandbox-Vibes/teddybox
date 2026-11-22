/* Box settings from teddyCloud (freshness-check response): applied when they
   change there, see freshness_box_changes(). The last values seen are kept
   in NVS, so a restart does not apply them again. */
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "freshness.h"
#include "settings.h"
#include "control.h"

static const char *TAG = "TCSET";
#define TCSET_NVS "TB_TCSET"

void freshness_box_settings_received(const freshness_settings_t *s)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    if (!settings_get()->tc_box_settings)
    {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(TCSET_NVS, NVS_READWRITE, &h) != ESP_OK)
    {
        return;
    }
    freshness_settings_t last;
    size_t len = sizeof(last);
    bool have_last = nvs_get_blob(h, "last", &last, &len) == ESP_OK && len == sizeof(last);
    if (have_last && !memcmp(&last, s, sizeof(last)))
    {
        nvs_close(h);
        return;
    }

    freshness_box_changes_t c;
    int n = freshness_box_changes(s, have_last ? &last : NULL, settings_get()->slap_level, &c);
    if (!have_last)
    {
        ESP_LOGI(TAG, "First box settings from teddyCloud stored; changes made there from now on are applied");
    }
    else if (n)
    {
        ESP_LOGI(TAG, "Applying %d box setting(s) changed in teddyCloud", n);
        if (c.volume_max_spk >= 0 || c.volume_max_hdp >= 0)
        {
            ESP_LOGI(TAG, "Max volume speaker %d %%, headphones %d %% (-1: unchanged)", c.volume_max_spk, c.volume_max_hdp);
            ctrl_request_volume_max(c.volume_max_spk, c.volume_max_hdp);
        }
        if (c.led_brightness >= 0)
        {
            ESP_LOGI(TAG, "LED brightness %d", c.led_brightness);
            ctrl_request_led_brightness(c.led_brightness);
        }
        if (c.slap_level >= 0 || c.slap_reverse >= 0)
        {
            ESP_LOGI(TAG, "Slap %d, reversed %d (-1: unchanged)", c.slap_level, c.slap_reverse);
            ctrl_request_motion(c.slap_level, -1, c.slap_reverse);
        }
    }
    nvs_set_blob(h, "last", s, sizeof(*s));
    nvs_commit(h);
    nvs_close(h);
}
