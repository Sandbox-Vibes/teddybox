/*
 * Content downloads (/v2/content/<uid>) into the SD card, with resume of a
 * partial file, automatic resume after a lost connection and the throttle
 * while a tonie plays. Requests come from playback and the nightly update
 * (cloud_content_download) and run in the cloud task (cloud_download_serve).
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cloud_internal.h"
#include "maint.h"
#include "wifi.h"
#include "playback.h"
#include "settings.h"
#include "taskwatch.h"
#include "memdiag.h"
#include "sd_retry.h"

static const char *TAG = "Cloud";

/* content downloads queued or running (verify and RTNL step aside for them) */
static int content_busy = 0;

/********************************************************/
/* content download handler                             */
/********************************************************/

esp_err_t cloud_create_directories(const char *file)
{
    char *tmp = strdup(file);
    char *p = tmp;

    ESP_LOGI(TAG, "[CDL] Create recursively dirs for '%s'", file);
    while ((p = strchr(p + 1, '/')))
    {
        *p = 0;
        ESP_LOGI(TAG, "[CDL]   Create '%s'", tmp);
        mkdir(tmp, S_IRWXU);
        *p = '/';
    }

    return ESP_OK;
}

esp_err_t cloud_content_status_cbr(void *ctx, int status_code)
{
    cloud_content_req_t *req = (cloud_content_req_t *)ctx;

    req->status_code = status_code;

    switch (req->status_code)
    {
    case 200:
        ESP_LOGI(TAG, "[CDL] HTTP %d received", req->status_code);

        req->state = CC_STATE_CONNECTED;
        xSemaphoreGive(req->update_sem);
        return ESP_OK;

    case 206:
        ESP_LOGI(TAG, "[CDL] HTTP %d received, partial content", req->status_code);

        req->state = CC_STATE_CONNECTED;
        xSemaphoreGive(req->update_sem);
        return ESP_OK;

    default:
        ESP_LOGE(TAG, "[CDL] Request failed for '%s': HTTP %d", req->location, req->status_code);

        req->state = CC_STATE_ERROR;
        xSemaphoreGive(req->update_sem);
        return ESP_FAIL;
    }
}

esp_err_t cloud_content_length_cbr(void *ctx, size_t range_start, size_t total_length)
{
    cloud_content_req_t *req = (cloud_content_req_t *)ctx;

    if (total_length == 0)
    {
        ESP_LOGE(TAG, "[CDL] Zero-sized or unknown size download, aborting");
        return ESP_FAIL;
    }

    if (PB_IS_STREAM_LENGTH(total_length) && !req->is_stream)
    {
        req->is_stream = true;
        ESP_LOGI(TAG, "[CDL] teddyCloud stream (up to %u bytes): not resumed, not kept", (unsigned)total_length);
    }
    if (req->is_stream && range_start > 0)
    {
        ESP_LOGE(TAG, "[CDL] Range answer for a stream, giving up");
        return ESP_FAIL;
    }

    /* the server may ignore our Range request and send everything again */
    bool resume = range_start > 0 && range_start == req->received;
    bool skip = false;
    if (!resume && req->resume_attempt > 0 && req->received > 0)
    {
        /* automatic resume while the tonie may be playing from this file: do
           not truncate it, drop the part we already have (only if it is the
           same content: same length) */
        if (total_length != req->content_length)
        {
            ESP_LOGE(TAG, "[CDL] Content changed during the download (%u instead of %u bytes)", (unsigned)total_length,
                     (unsigned)req->content_length);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "[CDL] Server sends from the start, dropping the first %u bytes", (unsigned)req->received);
        req->skip_left = req->received;
        skip = true;
        range_start = req->received;
        resume = true;
    }
    req->received = range_start;
    req->content_length = total_length;
    req->speed_start_us = esp_timer_get_time();
    req->speed_start_bytes = range_start;

    ESP_LOGI(TAG, "[CDL] download %u-%u to '%s'%s", req->received, req->content_length, req->tmpname,
             skip ? " (resume, skipping)" : resume ? " (resume)" : "");

    cloud_create_directories(req->tmpname);
    /* the cloud task's buffer: a cut off rest is fetched again on resume,
       nothing stays in it after the request */
    req->wbuf = cloud_bufs()->wbuf;
    req->wbuf_len = 0;
    req->sd_write_us = 0;
    req->synced = req->received;
    /* "wb+" would truncate the part we already have; the playback may use
       the handle, so it is replaced under the file lock */
    xSemaphoreTake(req->file_sem, portMAX_DELAY);
    if (req->handle)
    {
        fclose(req->handle);
    }
    req->handle = fopen(req->tmpname, resume ? "rb+" : "wb+");
    xSemaphoreGive(req->file_sem);
    if (!req->handle)
    {
        ESP_LOGE(TAG, "[CDL] Failed to open '%s'", req->tmpname);
        return ESP_FAIL;
    }

    req->state = CC_STATE_RECEIVING;

    return ESP_OK;
}

/* writes wbuf at "received"; an SD error (seen on the box: read timeout
   0x107 while FATFS looks up clusters) marks the FATFS file as broken, so
   the file is opened again before retrying. The reopened file has the size
   of the last sync; if that is less than "received", the download stops
   there and a resume continues at that size. */
static esp_err_t cloud_content_flush(cloud_content_req_t *req)
{
    if (!req->wbuf_len)
    {
        return ESP_OK;
    }
    while (!xSemaphoreTake(req->file_sem, 1000 / portTICK_PERIOD_MS))
    {
        ESP_LOGE(TAG, "[CDL] Timed out waiting for file lock...");
    }
    int64_t t0 = esp_timer_get_time();
    uint32_t end = req->received + req->wbuf_len;
    bool sync = end - req->synced >= CLOUD_SYNC_BYTES || end >= req->content_length;
    bool ok = false;
    for (int attempt = 0; attempt < SD_RETRIES && !ok; attempt++)
    {
        if (attempt)
        {
            ESP_LOGW(TAG, "[CDL] SD write at %u failed, reopening the file (attempt %d)", (unsigned)req->received, attempt + 1);
            if (req->handle)
            {
                fclose(req->handle);
            }
            vTaskDelay(pdMS_TO_TICKS(sd_retry_delay_ms(attempt)));
            req->handle = fopen(req->tmpname, "rb+");
            if (!req->handle)
            {
                continue;
            }
            long size = fseek(req->handle, 0, SEEK_END) == 0 ? ftell(req->handle) : -1;
            if (size < 0)
            {
                continue;
            }
            if ((uint32_t)size < req->received)
            {
                ESP_LOGW(TAG, "[CDL] File on the card ends at %ld instead of %u, stopping; a resume continues there",
                         size, (unsigned)req->received);
                req->received = size;
                req->synced = size;
                break;
            }
        }
        ok = req->handle && fseek(req->handle, req->received, SEEK_SET) == 0 &&
             fwrite(req->wbuf, req->wbuf_len, 1, req->handle) == 1 && fflush(req->handle) == 0 &&
             (!sync || fsync(fileno(req->handle)) == 0);
    }
    req->sd_write_us += esp_timer_get_time() - t0;
    if (ok)
    {
        /* only now the data is in the file */
        req->received = end;
        req->wbuf_len = 0;
        if (sync)
        {
            req->synced = end;
        }
    }
    xSemaphoreGive(req->file_sem);
    if (!ok)
    {
        ESP_LOGE(TAG, "[CDL] Write failed, bailing out");
        return ESP_FAIL;
    }
    xSemaphoreGive(req->update_sem);
    int64_t now = esp_timer_get_time();
    uint16_t kib_s = settings_get()->dl_playing_kib_s;
    if (!settings_get()->decoder_celt && (!kib_s || kib_s > 96))
    {
        /* the ESP-ADF decoder stuttered with faster downloads */
        kib_s = 96;
    }
    if (kib_s && pb_is_playing())
    {
        /* evenly spaced blocks: waiting here stops reading from the socket,
           TCP flow control slows the sender down */
        const int64_t block_us = CLOUD_WRITE_BLOCK * 1000000LL / (kib_s * 1024);
        if (req->next_block_us > now)
        {
            vTaskDelay(pdMS_TO_TICKS((req->next_block_us - now + 999) / 1000));
            now = esp_timer_get_time();
        }
        req->next_block_us = (req->next_block_us > now - block_us ? req->next_block_us : now) + block_us;
    }
    else
    {
        req->next_block_us = 0;
        /* give others the chance to react */
        vTaskDelay(1 / portTICK_RATE_MS);
    }
    return ESP_OK;
}

esp_err_t cloud_content_cbr(void *ctx, uint8_t *data, size_t length)
{
    taskwatch_feed(cloud_taskwatch);
    cloud_content_req_t *req = (cloud_content_req_t *)ctx;

    if (req->content_length == 0 || !req->handle)
    {
        ESP_LOGE(TAG, "[CDL] Nothing allocated... aborting");
        return ESP_FAIL;
    }

    if (req->abort)
    {
        req->state = CC_STATE_ABORTED;
        ESP_LOGE(TAG, "[CDL] aborting requested");
        return ESP_FAIL;
    }

    if (req->skip_left)
    {
        uint32_t n = length < req->skip_left ? length : req->skip_left;
        req->skip_left -= n;
        data += n;
        length -= n;
    }

    while (length > 0)
    {
        /* the first block may be shorter so that all others start at a 4 KB boundary */
        uint32_t target = CLOUD_WRITE_BLOCK - (req->received % CLOUD_WRITE_BLOCK);
        uint32_t n = target - req->wbuf_len;
        if (n > length)
        {
            n = length;
        }
        memcpy(req->wbuf + req->wbuf_len, data, n);
        req->wbuf_len += n;
        data += n;
        length -= n;
        if (req->wbuf_len == target && cloud_content_flush(req) != ESP_OK)
        {
            return ESP_FAIL;
        }
    }

    /* once a second; the speed only counts this connection (a resumed
       download starts in the middle, an earlier download is not part of it) */
    static int64_t last_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_us >= 1000000)
    {
        last_us = now_us;
        float elapsed = (now_us - req->speed_start_us) / 1e6f;
        uint32_t bytes = req->received - req->speed_start_bytes;
        float speed = elapsed > 0.5f ? bytes / elapsed : 0; /* bytes/s */
        float percent_complete = req->received * 100.0f / req->content_length;
        ESP_LOGI(TAG, "[CDL] %u bytes received (%.2f%%), Speed: %.1f KiB/s, ETA: %.0f s, SD write %d ms/s", (unsigned)req->received,
                 percent_complete, speed / 1024, speed > 0 ? (req->content_length - req->received) / speed : -1.0f,
                 (int)(req->sd_write_us / 1000));
        req->sd_write_us = 0;
    }
    return ESP_OK;
}

esp_err_t cloud_content_end_cbr(void *ctx)
{
    cloud_content_req_t *req = (cloud_content_req_t *)ctx;
    /* the last block, if the transfer is complete; a cut off rest is fetched again on resume */
    if (req->state == CC_STATE_RECEIVING && req->received + req->wbuf_len >= req->content_length)
    {
        cloud_content_flush(req);
    }
    ESP_LOGI(TAG, "[CDL] End transfer, %d/%d received", req->received, req->content_length);
    req->end_us = esp_timer_get_time();

/* playback handler initiated the download, it shall close handles
    if (req->handle)
    {
        fclose(req->handle);
        req->handle = NULL;
    }
*/
    if (req->state == CC_STATE_RECEIVING && req->received >= req->content_length)
    {
        if (req->is_stream)
        {
            /* the "stream" length was just a TAF file whose last page is
               short: it arrived completely, so it is kept like any tonie */
            ESP_LOGI(TAG, "[CDL] Complete file with a length that is no multiple of 4096: kept, no stream");
            req->is_stream = false;
        }
        req->state = CC_STATE_FINISHED;
    }
    else if (req->state == CC_STATE_RECEIVING && !req->abort && !req->is_stream)
    {
        /* connection lost midway: the cloud task tries again with Range */
        req->state = CC_STATE_RESUMING;
    }
    else
    {
        /* aborted or HTTP error (state already ERROR) */
        req->state = CC_STATE_ERROR;
    }
    xSemaphoreGive(req->update_sem);
    return ESP_OK;
}

/* a download that lost its connection midway is resumed with Range, while
   the requester waits (the tonie plays on from what is there). Boxine's CDN
   drops connections that teddyCloud passes through slowly
   (teddycloud#585); teddyCloud relies on the box to resume. */
esp_err_t cloud_process_request(cloud_content_req_t *content_req);
#define CLOUD_RESUME_TRIES 6
static const uint8_t resume_delay_s[CLOUD_RESUME_TRIES] = {2, 4, 8, 15, 30, 30};

static void cloud_resume_lost(cloud_content_req_t *req)
{
    int tries = 0;
    uint32_t last = req->received;
    while (req->state == CC_STATE_RESUMING)
    {
        if (req->received > last)
        {
            /* it made progress: start counting again */
            tries = 0;
            last = req->received;
        }
        if (tries >= CLOUD_RESUME_TRIES)
        {
            ESP_LOGE(TAG, "[CDL] Giving up after %d attempts at %u of %u bytes", tries, (unsigned)req->received,
                     (unsigned)req->content_length);
            req->state = CC_STATE_ERROR;
            break;
        }
        int delay_s = resume_delay_s[tries++];
        ESP_LOGW(TAG, "[CDL] Connection lost at %u of %u bytes, resuming in %d s (attempt %d of %d)",
                 (unsigned)req->received, (unsigned)req->content_length, delay_s, tries, CLOUD_RESUME_TRIES);
        for (int t = 0; t < delay_s * 10 && !req->abort; t++)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            taskwatch_feed(cloud_taskwatch);
        }
        if (req->abort)
        {
            req->state = CC_STATE_ABORTED;
            break;
        }
        if (!wifi_is_connected())
        {
            continue;
        }
        req->resume_attempt = tries;
        req->skip_left = 0;
        req->status_code = 0;
        cloud_process_request(req);
    }
    xSemaphoreGive(req->update_sem);
}

esp_err_t cloud_process_request(cloud_content_req_t *content_req)
{
    ESP_LOGI(TAG, "[CDL] Request for '%s' -> '%s'", content_req->location, content_req->filename);

    if (content_req->custom && active_backend == BACKEND_BOXINE)
    {
        /* the Boxine cloud only knows original tonies; asking for an own tag
           would only show it there (teddyCloud does the same: nocloud) */
        ESP_LOGW(TAG, "[CDL] %016llX is not an original tonie, not asked at the Boxine cloud",
                 (unsigned long long)content_req->nfc_uid);
        content_req->status_code = 404;
        content_req->state = CC_STATE_ERROR;
        xSemaphoreGive(content_req->update_sem);
        return ESP_FAIL;
    }

    http_parser_t http_parser_handler_ctx = {
        .http_status_cbr = &cloud_content_status_cbr,
        .http_data_cbr = &cloud_content_cbr,
        .http_end_cbr = &cloud_content_end_cbr,
        .content_length_cbr = &cloud_content_length_cbr,
        .ctx = content_req,
        .header_buffer = cloud_header_buffer()};

    cloud_req_t req = {
        .host = CLOUD_HOST,
        .port = CLOUD_PORT,
        .path = content_req->location,
        .auth = content_req->auth,
        .range_start = content_req->received,
        .data_received_cbr = &http_parser_received_cbr,
        .data_received_ctx = &http_parser_handler_ctx,
        .connection_closed_cbr = &http_parser_closed_cbr,
        .connection_closed_ctx = &http_parser_handler_ctx};

    content_req->state = CC_STATE_CONNECTING;
    esp_err_t ret = cloud_request(&req);


    /* connection failed before any response: make sure the waiting side gets an answer */
    if (content_req->resume_attempt > 0 && !content_req->abort && content_req->state != CC_STATE_FINISHED &&
        content_req->state != CC_STATE_ERROR)
    {
        /* an automatic resume that did not get through: the next attempt follows */
        content_req->state = CC_STATE_RESUMING;
    }
    else if (content_req->state != CC_STATE_FINISHED && content_req->state != CC_STATE_ERROR &&
             content_req->state != CC_STATE_RESUMING)
    {
        ESP_LOGE(TAG, "[CDL] Request for '%s' failed: no HTTP answer (%s)", content_req->location,
                 ret == ESP_OK ? "teddyCloud closed the connection, its log says why, e.g. no access to the Tonies cloud for a new tonie"
                               : "teddyCloud not reachable");
        content_req->state = CC_STATE_ERROR;
        xSemaphoreGive(content_req->update_sem);
    }

    return ret;
}


bool cloud_tag_is_custom(uint64_t uid, const uint8_t *token)
{
    if ((uid >> 40) != 0xE00403)
    {
        return true;
    }
    if (token)
    {
        for (int i = 0; i < 32; i++)
        {
            if (token[i])
            {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool cloud_download_busy(void)
{
    return __atomic_load_n(&content_busy, __ATOMIC_SEQ_CST) > 0;
}

cloud_content_req_t *cloud_content_download(uint64_t nfc_uid, const uint8_t *nfc_token)
{
    if (!cloud_request_queue)
    {
        ESP_LOGW(TAG, "[CDL] Cloud disabled, cannot download UID %llX", nfc_uid);
        return NULL;
    }

    cloud_content_req_t *req = calloc(1, sizeof(cloud_content_req_t));

    ESP_LOGI(TAG, "[CDL] Queue request for UID %llX", nfc_uid);

    /* fill input fields */
    req->nfc_uid = nfc_uid;
    req->custom = cloud_tag_is_custom(nfc_uid, nfc_token);

    /* fill working variables based on input fields */
    req->state = CC_STATE_INIT;
    req->update_sem = xSemaphoreCreateBinary();
    req->file_sem = xSemaphoreCreateBinary();
    req->status_code = 0;
    req->content_length = 0;
    req->received = 0;
    req->location = malloc(64);
    req->filename = pb_build_filename(nfc_uid);
    req->tmpname = malloc(strlen(req->filename) + 5);
    sprintf(req->tmpname, "%s.tmp", req->filename);
    req->auth = malloc(65);

    /* a partial download of an older TeddyBox version is in the final name */
    struct stat st;
    if (stat(req->tmpname, &st) != 0 && stat(req->filename, &st) == 0)
    {
        ESP_LOGI(TAG, "[CDL] Continuing the partial download as '%s'", req->tmpname);
        rename(req->filename, req->tmpname);
    }

    /* when the file already exists, do a partial download */
    if (stat(req->tmpname, &st) == 0)
    {
        /* only resume behind a valid TAF header (4 byte big endian length of
           the protobuf header, which fits into the first 4 KB block) */
        uint8_t be[4];
        FILE *f = st.st_size >= 4096 ? fopen(req->tmpname, "rb") : NULL;
        bool valid = f && fread(be, 4, 1, f) == 1 &&
                     ((uint32_t)be[0] << 24 | be[1] << 16 | be[2] << 8 | be[3]) <= 4096 - 4;
        if (f)
        {
            fclose(f);
        }
        if (valid && pb_file_is_stream(req->tmpname))
        {
            /* teddyCloud cannot continue a stream (teddycloud#606, #608), it starts anew */
            ESP_LOGI(TAG, "[CDL] Old stream data (%ld bytes), not resumed", (long)st.st_size);
        }
        else if (valid)
        {
            req->received = st.st_size;
            ESP_LOGI(TAG, "[CDL] Partial file, continue at %d", req->received);
        }
        else
        {
            ESP_LOGW(TAG, "[CDL] Existing file (%ld bytes) has no valid header, downloading it again", (long)st.st_size);
        }
    }

    xSemaphoreGive(req->file_sem);

    if (nfc_token)
    {
        char *auth_ptr = req->auth;
        for (int pos = 0; pos < 32; pos++)
        {
            auth_ptr += sprintf(auth_ptr, "%02X", nfc_token[pos]);
        }
    }
    else
    {
        strcpy(req->auth, "(error)");
    }

    if (nfc_uid)
    {
        char *location_ptr = req->location;
        location_ptr += sprintf(req->location, "/v2/content/");

        for (int i = 0; i < 8; ++i)
        {
            uint8_t byte = (req->nfc_uid >> (i * 8)) & 0xFF;
            location_ptr += sprintf(location_ptr, "%02X", byte);
        }
    }
    else
    {
        strcpy(req->filename, "(error)");
    }

    __atomic_add_fetch(&content_busy, 1, __ATOMIC_SEQ_CST);
    xQueueSend(cloud_request_queue, &req, portMAX_DELAY);

    return req;
}

cloud_content_state_t cloud_content_get_state(cloud_content_req_t *req)
{
    if (!req)
    {
        return CC_STATE_ERROR;
    }
    return req->state;
}

void cloud_content_cleanup(cloud_content_req_t *req)
{
    if (req->handle)
    {
        fclose(req->handle);
        req->handle = NULL;
    }
    /* complete: becomes the tonie file; partial: stays .tmp for a resume;
       a stream is not kept (it is never complete and cannot be resumed) */
    if (req->filename && req->tmpname && req->is_stream)
    {
        ESP_LOGI(TAG, "[CDL] Stream ended, deleting '%s' (%u bytes)", req->tmpname, (unsigned)req->received);
        unlink(req->tmpname);
    }
    else if (req->filename && req->tmpname)
    {
        pb_finish_tmp(req->filename);
    }
    free(req->tmpname);
    /* req->wbuf belongs to the cloud task */
    vSemaphoreDelete(req->update_sem);
    free(req->filename);
    free(req);
}

static void cloud_fail_request(cloud_content_req_t *req, const char *reason)
{
    ESP_LOGW(TAG, "[CDL] Not downloading '%s': %s", req->location, reason);
    req->state = CC_STATE_ERROR;
    xSemaphoreGive(req->update_sem);
}


bool cloud_download_serve(cloud_content_req_t *req, bool cloud_available)
{
        bool connection_failed = false;
    if (req->abort)
    {
        cloud_fail_request(req, "aborted");
    }
    else if (!wifi_is_connected())
    {
        cloud_fail_request(req, "no WiFi");
    }
    else if (!cloud_available)
    {
        cloud_fail_request(req, "cloud not reachable");
    }
    else if (cloud_process_request(req) != ESP_OK && req->status_code == 0)
    {
        connection_failed = true;
    }
    else
    {
        cloud_resume_lost(req);
    }
    __atomic_sub_fetch(&content_busy, 1, __ATOMIC_SEQ_CST);
    /* last access to req, the requester frees it after this */
    req->done = true;
    return connection_failed;
}
