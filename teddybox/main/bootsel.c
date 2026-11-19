#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_spi_flash.h"

#include "bootsel.h"

static const char *TAG = "BootSel";

#define OFW_PREFIX "toniebox-esp32-"
#define OFW_EU "toniebox-esp32-eu"
#define OFW_PD "toniebox-esp32-pd"
#define MAX_SLOTS 3

typedef struct
{
    const esp_partition_t *part;
    bool valid;
    esp_app_desc_t desc;
} slot_t;

static slot_t slots[MAX_SLOTS];
static int slot_count = 0;

static const slot_t *find_slot(const esp_partition_t *part)
{
    for (int i = 0; i < slot_count; i++)
    {
        if (slots[i].part && part && slots[i].part->address == part->address)
        {
            return &slots[i];
        }
    }
    return NULL;
}

static bool slot_is(const slot_t *s, const char *project)
{
    return s && s->valid && !strcmp(s->desc.project_name, project);
}

void bootsel_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    const esp_partition_t *running = esp_ota_get_running_partition();
    slot_count = 0;

    for (int i = 0; i < MAX_SLOTS; i++)
    {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + i, NULL);
        if (!p)
        {
            continue;
        }
        slot_t *s = &slots[slot_count++];
        s->part = p;
        s->valid = esp_ota_get_partition_description(p, &s->desc) == ESP_OK;

        ESP_LOGI(TAG, "  %s @0x%06x: %s %s%s", p->label, p->address,
                 s->valid ? s->desc.project_name : "(empty)",
                 s->valid ? s->desc.version : "",
                 (running && running->address == p->address) ? "  <- running" : "");
    }

    if (!bootsel_get_original())
    {
        ESP_LOGW(TAG, "No original EU firmware found in any slot");
    }
}

bool bootsel_is_original(const esp_partition_t *part)
{
    const slot_t *s = find_slot(part);
    return s && s->valid && !strncmp(s->desc.project_name, OFW_PREFIX, strlen(OFW_PREFIX));
}

const esp_partition_t *bootsel_get_original(void)
{
    for (int i = 0; i < slot_count; i++)
    {
        if (slot_is(&slots[i], OFW_EU))
        {
            return slots[i].part;
        }
    }
    return NULL;
}

bool bootsel_get_original_desc(esp_app_desc_t *desc)
{
    for (int i = 0; i < slot_count; i++)
    {
        if (slot_is(&slots[i], OFW_EU))
        {
            *desc = slots[i].desc;
            return true;
        }
    }
    return false;
}

const esp_partition_t *bootsel_get_update_target(bool allow_overwrite_pd)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    /* 1st choice: an empty slot or one with something else than the original firmware */
    for (int i = 0; i < slot_count; i++)
    {
        if (slots[i].part->address == running->address)
        {
            continue;
        }
        if (!bootsel_is_original(slots[i].part))
        {
            return slots[i].part;
        }
    }

    /* 2nd choice: the PD app, only if explicitly allowed */
    if (allow_overwrite_pd)
    {
        for (int i = 0; i < slot_count; i++)
        {
            if (slots[i].part->address != running->address && slot_is(&slots[i], OFW_PD))
            {
                ESP_LOGW(TAG, "Update will overwrite the original PD app in %s", slots[i].part->label);
                return slots[i].part;
            }
        }
    }

    return NULL;
}

/* esp_ota_select_entry_t layout: seq (4), label (20), state (4), crc (4); one entry per 4 KiB sector */
#define OTADATA_STATE_OFFSET 24

static void otadata_mark_newest_valid(void)
{
    const esp_partition_t *otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!otadata)
    {
        return;
    }
    uint8_t entry[2][32];
    if (esp_partition_read(otadata, 0, entry[0], 32) != ESP_OK || esp_partition_read(otadata, SPI_FLASH_SEC_SIZE, entry[1], 32) != ESP_OK)
    {
        return;
    }
    uint32_t seq0, seq1;
    memcpy(&seq0, entry[0], 4);
    memcpy(&seq1, entry[1], 4);
    /* the entry just written has the higher sequence number (0xFFFFFFFF = empty) */
    int newest = (seq1 != 0xFFFFFFFF && (seq0 == 0xFFFFFFFF || seq1 > seq0)) ? 1 : 0;

    uint32_t valid = ESP_OTA_IMG_VALID;
    memcpy(&entry[newest][OTADATA_STATE_OFFSET], &valid, 4);
    /* the CRC only covers the sequence number; if power fails between erase and
       write, the bootloader uses the other (TeddyBox) entry */
    if (esp_partition_erase_range(otadata, newest * SPI_FLASH_SEC_SIZE, SPI_FLASH_SEC_SIZE) == ESP_OK &&
        esp_partition_write(otadata, newest * SPI_FLASH_SEC_SIZE, entry[newest], 32) == ESP_OK)
    {
        ESP_LOGI(TAG, "Original firmware marked as valid, no probation");
    }
}

esp_err_t bootsel_boot_original(void)
{
    const esp_partition_t *ofw = bootsel_get_original();
    if (!ofw)
    {
        ESP_LOGE(TAG, "Cannot boot original firmware, none found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_ota_set_boot_partition(ofw);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        return err;
    }
    /* with rollback enabled the original app would start "on probation" and the
       bootloader would come back to TeddyBox unless it confirms itself in time.
       It is a known good image, so mark it valid right away. */
    otadata_mark_newest_valid();

    ESP_LOGW(TAG, "Rebooting into original firmware in %s", ofw->label);
    esp_restart();
    return ESP_OK;
}
