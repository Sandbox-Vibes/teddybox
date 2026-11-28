/*
 * Backup in the setup mode: the box certificates (for teddyCloud) and the
 * complete flash (original firmware, NVS with WiFi data, certificates), as
 * a download or onto the SD card.
 *
 * The setup hotspot is open, so the downloads are only released after
 * somebody presses the small ear on the box (valid for 10 minutes).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_rom_crc.h"
#include "esp_http_server.h"

#include "board.h"
#include "ledman.h"
#include "backup.h"
#include "backup_zip.h"
#include "setup_portal.h"

static const char *TAG = "Backup";

#define UNLOCK_WAIT_US (60 * 1000000LL)      /* time to press the ear */
#define UNLOCK_VALID_US (10 * 60 * 1000000LL) /* downloads allowed afterwards */
#define CHUNK 4096

static const char *const cert_files[] = {"ca.der", "client.der", "private.der"};
#define CERT_DIR "/spiflash/cert/"
#define SD_DIR "/sdcard/teddybox_backup"

/* written by the web server and the unlock task: 64 bit, so read and
   written under a lock (a 32 bit core needs two stores) */
static portMUX_TYPE bk_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t wait_until = 0;
static int64_t unlocked_until = 0;

static int64_t bk_get(const int64_t *v)
{
    taskENTER_CRITICAL(&bk_lock);
    int64_t r = *v;
    taskEXIT_CRITICAL(&bk_lock);
    return r;
}

static void bk_set(int64_t *v, int64_t value)
{
    taskENTER_CRITICAL(&bk_lock);
    *v = value;
    taskEXIT_CRITICAL(&bk_lock);
}

static bool unlocked(void)
{
    return esp_timer_get_time() < bk_get(&unlocked_until);
}

static void box_id(char *out, size_t size)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, size, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* polls the small ear while somebody waits for the release */
static void unlock_task(void *arg)
{
    ledman_change("fadeblink blue");
    bool was_pressed = audio_board_ear_small();
    while (esp_timer_get_time() < bk_get(&wait_until))
    {
        bool pressed = audio_board_ear_small();
        /* a new press, not an ear that was already held */
        if (pressed && !was_pressed)
        {
            bk_set(&unlocked_until, esp_timer_get_time() + UNLOCK_VALID_US);
            ESP_LOGI(TAG, "Released by the small ear for %lld min", UNLOCK_VALID_US / 60000000);
            break;
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    bk_set(&wait_until, 0);
    ledman_change(SETUP_LED_PATTERN);
    vTaskDelete(NULL);
}

static esp_err_t send_status(httpd_req_t *req)
{
    char json[96];
    int64_t now = esp_timer_get_time();
    snprintf(json, sizeof(json), "{\"unlocked\":%s,\"waiting\":%s,\"flash_size\":%lu}", unlocked() ? "true" : "false",
             now < bk_get(&wait_until) ? "true" : "false", (unsigned long)backup_flash_size());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t unlock_handler(httpd_req_t *req)
{
    if (!unlocked() && esp_timer_get_time() >= bk_get(&wait_until))
    {
        bk_set(&wait_until, esp_timer_get_time() + UNLOCK_WAIT_US);
        ESP_LOGI(TAG, "Waiting for the small ear");
        if (xTaskCreate(unlock_task, "[TB] backup", 2560, NULL, 5, NULL) != pdPASS)
        {
            bk_set(&wait_until, 0);
        }
    }
    return send_status(req);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    return send_status(req);
}

static esp_err_t refuse(httpd_req_t *req)
{
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_sendstr(req, "Press the small ear on the box first");
}

uint32_t backup_flash_size(void)
{
    uint32_t size = 0;
    esp_flash_get_size(NULL, &size);
    return size;
}

/* reads the three certificates; returns how many were found */
static int read_certs(uint8_t *bufs[3], size_t sizes[3])
{
    int found = 0;
    for (int i = 0; i < 3; i++)
    {
        char path[48];
        snprintf(path, sizeof(path), CERT_DIR "%s", cert_files[i]);
        bufs[i] = NULL;
        sizes[i] = 0;
        FILE *f = fopen(path, "rb");
        if (!f)
        {
            continue;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 0 && len < 8192 && (bufs[i] = malloc(len)) && fread(bufs[i], 1, len, f) == (size_t)len)
        {
            sizes[i] = len;
            found++;
        }
        else
        {
            free(bufs[i]);
            bufs[i] = NULL;
        }
        fclose(f);
    }
    return found;
}

static esp_err_t certs_handler(httpd_req_t *req)
{
    if (!unlocked())
    {
        return refuse(req);
    }
    uint8_t *bufs[3];
    size_t sizes[3];
    if (read_certs(bufs, sizes) != 3)
    {
        for (int i = 0; i < 3; i++)
        {
            free(bufs[i]);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Certificates not found");
        return ESP_FAIL;
    }
    size_t zsize = sizes[0] + sizes[1] + sizes[2] + 3 * (30 + 46 + 2 * 16) + 22;
    uint8_t *zip = malloc(zsize);
    int len = zip ? backup_zip(cert_files, (const uint8_t *const *)bufs, sizes, 3, zip, zsize) : -1;
    for (int i = 0; i < 3; i++)
    {
        free(bufs[i]);
    }
    if (len < 0)
    {
        free(zip);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    char id[13], disp[80];
    box_id(id, sizeof(id));
    snprintf(disp, sizeof(disp), "attachment; filename=\"toniebox-%s-certs.zip\"", id);
    httpd_resp_set_type(req, "application/zip");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    esp_err_t err = httpd_resp_send(req, (const char *)zip, len);
    free(zip);
    ESP_LOGI(TAG, "Certificates downloaded");
    return err;
}

static esp_err_t flash_handler(httpd_req_t *req)
{
    if (!unlocked())
    {
        return refuse(req);
    }
    uint32_t size = backup_flash_size();
    uint8_t *buf = malloc(CHUNK);
    if (!size || !buf)
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    char id[13], disp[80], len[16];
    box_id(id, sizeof(id));
    snprintf(disp, sizeof(disp), "attachment; filename=\"toniebox-%s-flash.bin\"", id);
    snprintf(len, sizeof(len), "%lu", (unsigned long)size);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    ESP_LOGI(TAG, "Sending the flash (%lu bytes)", (unsigned long)size);
    esp_err_t err = ESP_OK;
    for (uint32_t off = 0; off < size && err == ESP_OK; off += CHUNK)
    {
        err = esp_flash_read(NULL, buf, off, CHUNK);
        if (err == ESP_OK)
        {
            err = httpd_resp_send_chunk(req, (const char *)buf, CHUNK);
        }
        setup_portal_touch();
    }
    free(buf);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Flash download aborted: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Flash downloaded");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(data, 1, len, f) == len;
    if (f)
    {
        ok = (fclose(f) == 0) && ok;
    }
    return ok;
}

/* certificates and flash into /sdcard/teddybox_backup/<mac>/ */
static esp_err_t sd_handler(httpd_req_t *req)
{
    if (!unlocked())
    {
        return refuse(req);
    }
    char id[13], dir[64], path[96];
    box_id(id, sizeof(id));
    snprintf(dir, sizeof(dir), SD_DIR "/%s", id);
    mkdir(SD_DIR, 0775);
    mkdir(dir, 0775);

    bool ok = true;
    uint8_t *bufs[3];
    size_t sizes[3];
    ok = read_certs(bufs, sizes) == 3;
    for (int i = 0; i < 3; i++)
    {
        if (bufs[i])
        {
            snprintf(path, sizeof(path), "%s/%s", dir, cert_files[i]);
            ok = write_file(path, bufs[i], sizes[i]) && ok;
            free(bufs[i]);
        }
    }

    uint32_t size = backup_flash_size();
    uint8_t *buf = malloc(CHUNK);
    snprintf(path, sizeof(path), "%s/flash.bin", dir);
    FILE *f = buf ? fopen(path, "wb") : NULL;
    ok = ok && f;
    for (uint32_t off = 0; f && off < size && ok; off += CHUNK)
    {
        ok = esp_flash_read(NULL, buf, off, CHUNK) == ESP_OK && fwrite(buf, 1, CHUNK, f) == CHUNK;
        setup_portal_touch();
    }
    if (f)
    {
        ok = (fclose(f) == 0) && ok;
    }
    free(buf);
    ESP_LOGI(TAG, "Backup to %s %s", dir, ok ? "done" : "FAILED");

    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"dir\":\"teddybox_backup/%s\"}", ok ? "true" : "false", id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

#ifdef TEDDYBOX_QEMU_TEST
/* QEMU has no ears */
void backup_test_unlock(void)
{
    unlocked_until = esp_timer_get_time() + UNLOCK_VALID_US;
}
#endif

esp_err_t backup_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/backup/unlock", .method = HTTP_POST, .handler = unlock_handler},
        {.uri = "/api/backup/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/backup/sd", .method = HTTP_POST, .handler = sd_handler},
        {.uri = "/backup/certs.zip", .method = HTTP_GET, .handler = certs_handler},
        {.uri = "/backup/flash.bin", .method = HTTP_GET, .handler = flash_handler},
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
    {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    return ESP_OK;
}
