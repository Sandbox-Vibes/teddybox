#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdbool.h>

#include "esp_log.h"

#include "sfx.h"
#include "playback.h"

static const char *TAG = "SFX";

#define SFX_MAX_ENTRIES 32
#define SFX_COPY_BUF 2048

/*
 * Returns field 2 (num_bytes) of a TonieboxAudioFileHeader. Parsed by hand
 * because the empty placeholder entries lack the required _fill field and
 * would be rejected by the generated protobuf-c unpacker.
 */
static bool taf_num_bytes(const uint8_t *buf, size_t len, uint64_t *num_bytes)
{
    size_t pos = 0;
    while (pos < len)
    {
        uint64_t key = 0;
        for (int shift = 0; pos < len; shift += 7)
        {
            uint8_t b = buf[pos++];
            key |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80) || shift > 56)
            {
                break;
            }
        }
        uint32_t field = key >> 3, type = key & 7;
        uint64_t value = 0;

        if (type == 0 || type == 2)
        {
            for (int shift = 0; pos < len; shift += 7)
            {
                uint8_t b = buf[pos++];
                value |= (uint64_t)(b & 0x7f) << shift;
                if (!(b & 0x80) || shift > 56)
                {
                    break;
                }
            }
        }
        else
        {
            return false; /* fixed32/64 do not occur in this header */
        }

        if (type == 0 && field == 2)
        {
            *num_bytes = value;
            return true;
        }
        if (type == 2)
        {
            if (value > len - pos)
            {
                return false;
            }
            pos += value;
        }
    }
    return false;
}

static esp_err_t sfx_copy(FILE *src, size_t length, const char *dst_path)
{
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dst_path);

    FILE *dst = fopen(tmp_path, "wb");
    if (!dst)
    {
        ESP_LOGE(TAG, "Cannot create '%s'", tmp_path);
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(SFX_COPY_BUF);
    if (!buf)
    {
        fclose(dst);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    while (length > 0)
    {
        size_t chunk = length > SFX_COPY_BUF ? SFX_COPY_BUF : length;
        if (fread(buf, chunk, 1, src) != 1 || fwrite(buf, chunk, 1, dst) != 1)
        {
            ret = ESP_FAIL;
            break;
        }
        length -= chunk;
    }
    free(buf);
    fclose(dst);

    if (ret != ESP_OK)
    {
        unlink(tmp_path);
        return ret;
    }
    unlink(dst_path);
    if (rename(tmp_path, dst_path) != 0)
    {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sfx_install(uint8_t lang_id)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    FILE *f = fopen(SFX_BIN_PATH, "rb");
    if (!f)
    {
        ESP_LOGW(TAG, "No %s found", SFX_BIN_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    char dir[40];
    snprintf(dir, sizeof(dir), "/sdcard/CONTENT/%08X", lang_id);
    mkdir("/sdcard/CONTENT", S_IRWXU);
    mkdir(dir, S_IRWXU);

    uint8_t *hdr = malloc(TONIEFILE_FRAME_SIZE);
    if (!hdr)
    {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    int installed = 0, present = 0;
    long offset = 0;
    esp_err_t ret = ESP_OK;

    for (int id = 0; id < SFX_MAX_ENTRIES; id++)
    {
        if (fseek(f, offset, SEEK_SET) != 0 || fread(hdr, 4, 1, f) != 1)
        {
            break; /* end of container */
        }
        uint32_t hdr_len = (hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
        if (hdr_len == 0 || hdr_len > TONIEFILE_FRAME_SIZE - 4 || fread(hdr + 4, hdr_len, 1, f) != 1)
        {
            ESP_LOGE(TAG, "Invalid header for ID %d at offset %ld", id, offset);
            ret = ESP_ERR_INVALID_STATE;
            break;
        }

        uint64_t num_bytes = 0;
        if (!taf_num_bytes(hdr + 4, hdr_len, &num_bytes) || num_bytes > 16 * 1024 * 1024)
        {
            ESP_LOGE(TAG, "Cannot decode header for ID %d", id);
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        size_t audio_len = num_bytes;

        size_t entry_len = 4 + hdr_len + audio_len;
        if (audio_len > 0)
        {
            char path[56];
            snprintf(path, sizeof(path), "%s/%08X", dir, id);

            struct stat st;
            /* never overwrite an existing file, it may be a user supplied sound.
               installs go through a .tmp file, so there are no partial leftovers */
            if (stat(path, &st) == 0)
            {
                present++;
            }
            else if (fseek(f, offset, SEEK_SET) == 0 && sfx_copy(f, entry_len, path) == ESP_OK)
            {
                installed++;
            }
            else
            {
                ESP_LOGE(TAG, "Failed to install ID %d to '%s'", id, path);
            }
        }
        offset += entry_len;
    }

    free(hdr);
    fclose(f);

    ESP_LOGI(TAG, "System sounds in %s: %d installed, %d already present", dir, installed, present);
    return ret;
}
