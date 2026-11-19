#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_app_format.h"
#include "esp_partition.h"

#include "ota.h"
#include "bootsel.h"

static const char *TAG = "OTA";

#define PROJECT_NAME "teddybox"

const char *ota_check_header(const uint8_t *data, size_t len, size_t total_len, esp_app_desc_t *desc)
{
    const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (len < desc_off + sizeof(esp_app_desc_t))
    {
        return "Datei ist zu klein";
    }

    const esp_image_header_t *hdr = (const esp_image_header_t *)data;
    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC)
    {
        return "Keine ESP32-Firmware";
    }
    if (hdr->chip_id != ESP_CHIP_ID_ESP32S3)
    {
        return "Firmware ist nicht für den ESP32-S3";
    }

    memcpy(desc, data + desc_off, sizeof(*desc));
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD)
    {
        return "Firmware ohne App-Beschreibung";
    }
    desc->project_name[sizeof(desc->project_name) - 1] = 0;
    desc->version[sizeof(desc->version) - 1] = 0;
    if (strcmp(desc->project_name, PROJECT_NAME))
    {
        /* also keeps original Boxine apps out of the TeddyBox slots */
        return "Das ist keine TeddyBox-Firmware";
    }
    if (total_len == 0)
    {
        return "Leere Datei";
    }
    return NULL;
}

const char *ota_session_begin(ota_session_t *s, size_t image_len, bool allow_pd, bool *needs_pd)
{
    memset(s, 0, sizeof(*s));
    *needs_pd = false;

    s->target = bootsel_get_update_target(false);
    if (!s->target)
    {
        if (!bootsel_get_update_target(true))
        {
            return "Kein freier Slot für ein Update";
        }
        if (!allow_pd)
        {
            *needs_pd = true;
            return "Für das erste Update wird der Slot der Notfall-App (PD) gebraucht";
        }
        s->target = bootsel_get_update_target(true);
    }
    if (image_len > s->target->size)
    {
        return "Firmware ist zu groß für den Slot";
    }

    esp_err_t err = esp_ota_begin(s->target, image_len, &s->handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        return "Slot konnte nicht vorbereitet werden";
    }
    s->started = true;
    ESP_LOGI(TAG, "Writing %u bytes to %s @0x%06x", (unsigned)image_len, s->target->label, s->target->address);
    return NULL;
}

const char *ota_session_write(ota_session_t *s, const uint8_t *data, size_t len)
{
    esp_err_t err = esp_ota_write(s->handle, data, len);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
        return "Schreiben fehlgeschlagen";
    }
    s->written += len;
    return NULL;
}

const char *ota_session_finish(ota_session_t *s)
{
    s->started = false;
    esp_err_t err = esp_ota_end(s->handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
        return err == ESP_ERR_OTA_VALIDATE_FAILED ? "Firmware ist beschädigt (Prüfsumme falsch)" : "Prüfung fehlgeschlagen";
    }
    err = esp_ota_set_boot_partition(s->target);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        return "Konnte die neue Firmware nicht als Start auswählen";
    }
    ESP_LOGW(TAG, "Update in %s installed, it boots on probation", s->target->label);
    return NULL;
}

void ota_session_abort(ota_session_t *s)
{
    if (s->started)
    {
        esp_ota_abort(s->handle);
        s->started = false;
    }
}

void ota_confirm_running(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        {
            ESP_LOGW(TAG, "New firmware in %s confirmed, rollback cancelled", running->label);
        }
    }
}
