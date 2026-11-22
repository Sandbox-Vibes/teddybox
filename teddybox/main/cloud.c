
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_tls.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "esp_timer.h"
#include "memdiag.h"
#include "mempeak.h"
#include "cloud.h"
#include "wifi.h"
#include "playback.h"
#include "settings.h"
#include "freshness.h"
#include "tonie_names_tc.h"
#include "taskwatch.h"
#include "bootsel.h"
#include "sd_retry.h"

/*
 * Up to two backends: teddyCloud (preferred) and, only if boxine_fallback
 * is enabled, the Boxine cloud with the original CA from the assets
 * partition. The cloud task picks the first that answers /v1/time.
 */
typedef struct
{
    const char *name;
    const char *host;
    uint16_t port;
    uint8_t *ca;
    size_t ca_len;
    /* server name for SNI and certificate check, NULL = neither (teddyCloud's
       certificate is issued to "TeddyCloud Server", not to a host name) */
    const char *tls_name;
} cloud_backend_t;

#define BACKEND_TEDDYCLOUD 0
#define BACKEND_BOXINE 1
/* while on Boxine, check every few minutes whether teddyCloud is back */
#ifndef BACKEND_RETRY_PREFERRED_MS
#define BACKEND_RETRY_PREFERRED_MS (5 * 60 * 1000)
#endif

static cloud_backend_t backends[2];
static volatile int active_backend = BACKEND_TEDDYCLOUD;
/* teddyCloud answers (time sync worked) and is the backend in use */
static volatile bool teddycloud_up = false;
static char user_agent[64];

#define CLOUD_HOST (backends[active_backend].host)
#define CLOUD_PORT (backends[active_backend].port)

/* download write block = TAF block, and how often an SD access is retried */
#define CLOUD_WRITE_BLOCK 4096
/* while a tonie plays the download is limited, the SD card and the CPU are
   shared with the playback (about 12 KiB/s): at full speed (250 KiB/s with
   IDF 5, ESP-ADF decoder at 160 MHz) the sound stuttered. Limit from
   teddybox.ini "download_playing" (default 0 = unlimited, about 300 KiB/s
   with CELT at 160 MHz: decoder 40.7 % of one core, clean sound; decoder =
   adf: at most 96). Without
   playback (nightly update) full speed. */
#define CLOUD_SYNC_BYTES (128 * 1024)

static const char *TAG = "Cloud";

uint8_t *client_der = NULL;
uint8_t *private_der = NULL;
size_t client_der_len = 0;
size_t private_der_len = 0;
static esp_app_desc_t running_app_info;
static QueueHandle_t cloud_request_queue;

/********************************************************/
/* TLS connection handling                              */
/********************************************************/

esp_tls_t *cloud_connect_teddycloud(void)
{
    /* never the Boxine cloud */
    if (!teddycloud_up || !client_der)
    {
        return NULL;
    }
    cloud_backend_t *b = &backends[BACKEND_TEDDYCLOUD];
    esp_tls_cfg_t cfg = {
        .cacert_buf = b->ca,
        .cacert_bytes = b->ca_len,
        .clientcert_buf = client_der,
        .clientcert_bytes = client_der_len,
        .clientkey_buf = private_der,
        .clientkey_bytes = private_der_len,
        .timeout_ms = 10000,
        .common_name = b->tls_name,
        .skip_common_name = b->tls_name == NULL};
    bool peak = mempeak_enter("TLS handshake", 8000);
    esp_tls_t *tls = esp_tls_init();
    if (tls && esp_tls_conn_new_sync(b->host, strlen(b->host), b->port, &cfg, tls) != 1)
    {
        esp_tls_conn_destroy(tls);
        tls = NULL;
    }
    mempeak_leave(peak);
    return tls;
}

/* the socket timeout is 10 s: 6 idle reads = 1 minute without any data */
#define CLOUD_READ_IDLE_MAX 6
static int cloud_taskwatch = -1;

static esp_err_t cloud_request(cloud_req_t *req)
{
    esp_err_t ret = ESP_FAIL;

    char *auth_line = strdup("");
    char *range_line = strdup("");
    char *request;
    char *url;

    if (req->auth)
    {
        free(auth_line);
        asprintf(&auth_line, "Authorization: BD %s\r\n", req->auth);
    }
    if (req->range_start)
    {
        free(range_line);
        asprintf(&range_line, "Range: bytes=%" PRIu32 "-\r\n", req->range_start);
    }
    asprintf(&url, "https://%s:%d%s", req->host, req->port, req->path);
    ESP_LOGI(TAG, "Connect to %s...", url);

    char body_line[48] = "";
    if (req->body)
    {
        snprintf(body_line, sizeof(body_line), "Content-Length: %u\r\n", (unsigned)req->body_len);
    }

    asprintf(&request, "%s %s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "%s"
                       "%s"
                       "%s"
                       "User-Agent: %s\r\n"
                       "Connection: close\r\n"
                       "\r\n",
             req->body ? "POST" : "GET", req->path, req->host, auth_line, range_line, body_line, user_agent);

    // ESP_LOGI(TAG, "Header %s...", request);
    esp_tls_cfg_t cfg = {
        .cacert_buf = backends[active_backend].ca,
        .cacert_bytes = backends[active_backend].ca_len,
        .clientcert_buf = client_der,
        .clientcert_bytes = client_der_len,
        .clientkey_buf = private_der,
        .clientkey_bytes = private_der_len,
        .timeout_ms = 10000,
        .common_name = backends[active_backend].tls_name,
        .skip_common_name = backends[active_backend].tls_name == NULL};

    uint8_t *receive_buffer = malloc(HTTP_RECEIVE_SIZE);
    if (!receive_buffer)
    {
        ESP_LOGE(TAG, "Allocation failed...");
        return ESP_FAIL;
    }

    /* the handshake is one of the two big memory peaks, see mempeak.h */
    bool peak = mempeak_enter("TLS handshake", 8000);
    esp_tls_t *tls = esp_tls_init();
    if (tls && esp_tls_conn_http_new_sync(url, &cfg, tls) != 1)
    {
        esp_tls_conn_destroy(tls);
        tls = NULL;
    }
    mempeak_leave(peak);
    if (!tls)
    {
        ESP_LOGE(TAG, "Connection failed...");
        goto exit;
    }

    ESP_LOGI(TAG, "Connection to '%s' established...", req->host);

    size_t written_bytes = 0;
    size_t request_len = strlen(request);
    do
    {
        int ret = esp_tls_conn_write(tls, request + written_bytes, request_len - written_bytes);
        if (ret >= 0)
        {
            written_bytes += ret;
        }
        else if (ret != ESP_TLS_ERR_SSL_WANT_READ && ret != ESP_TLS_ERR_SSL_WANT_WRITE)
        {
            ESP_LOGE(TAG, "esp_tls_conn_write returned: [0x%02X](%s)", ret, esp_err_to_name(ret));
            goto exit;
        }
    } while (written_bytes < request_len);

    written_bytes = 0;
    while (req->body && written_bytes < req->body_len)
    {
        int ret = esp_tls_conn_write(tls, req->body + written_bytes, req->body_len - written_bytes);
        if (ret >= 0)
        {
            written_bytes += ret;
        }
        else if (ret != ESP_TLS_ERR_SSL_WANT_READ && ret != ESP_TLS_ERR_SSL_WANT_WRITE)
        {
            ESP_LOGE(TAG, "esp_tls_conn_write (body) returned: [0x%02X](%s)", ret, esp_err_to_name(ret));
            goto exit;
        }
    }

    bool connected = true;
    int idle = 0;

    ret = ESP_OK;

    while (connected)
    {
        int len = esp_tls_conn_read(tls, receive_buffer, HTTP_RECEIVE_SIZE);

        if (len == ESP_TLS_ERR_SSL_WANT_WRITE || len == ESP_TLS_ERR_SSL_WANT_READ)
        {
            /* the socket timeout (timeout_ms) passed without data; a server
               that stays silent must not keep the cloud task forever */
            if (++idle >= CLOUD_READ_IDLE_MAX)
            {
                ESP_LOGE(TAG, "No data for %d s, giving up the connection", idle * 10);
                if (req->connection_closed_cbr)
                {
                    req->connection_closed_cbr(req->connection_closed_ctx);
                }
                ret = ESP_FAIL;
                connected = false;
                continue;
            }
            ESP_LOGI(TAG, "TLS socket idle");
            continue;
        }
        idle = 0;

        if (len < 0)
        {
            ESP_LOGE(TAG, "esp_tls_conn_read returned [-0x%02X](%s)", -len, esp_err_to_name(len));
            if (req->connection_closed_cbr)
            {
                req->connection_closed_cbr(req->connection_closed_ctx);
            }
            ret = ESP_FAIL;
            connected = false;
            continue;
        }

        if (len == 0)
        {
            connected = false;
            continue;
        }

        if (req->data_received_cbr)
        {
            esp_err_t cbr_ret = req->data_received_cbr(req->data_received_ctx, receive_buffer, len);

            switch (cbr_ret)
            {
            case ESP_OK:
                break;

            case ESP_FAIL:
                ret = ESP_FAIL;
                connected = false;
                continue;

            case CBR_CLOSE_OK:
                connected = false;
                continue;
            }
        }
    }

    ESP_LOGI(TAG, "connection closed");
    memdiag_log("after TLS");
    if (req->connection_closed_cbr)
    {
        req->connection_closed_cbr(req->connection_closed_ctx);
    }

exit:
    esp_tls_conn_destroy(tls);

    free(auth_line);
    free(range_line);
    free(request);
    free(url);
    free(receive_buffer);

    return ret;
}

/********************************************************/
/* time request handler                                 */
/********************************************************/

esp_err_t cloud_set_time_cbr(void *ctx, uint8_t *data, size_t length)
{
    if (length <= 5 || length > 32)
    {
        return ESP_OK;
    }

    char str[33];
    memcpy(str, data, length);
    str[length] = 0;
    ESP_LOGI(TAG, "Received time string: %s", str);

    time_t unixTime = atol(str);
    if (unixTime == 0 && strcmp(str, "0") != 0)
    {
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = unixTime,
        .tv_usec = 0};

    if (settimeofday(&tv, NULL) == -1)
    {
        return ESP_FAIL;
    }

    time_t currentTime;
    struct tm *timeInfo;
    char buffer[80];

    time(&currentTime);
    timeInfo = localtime(&currentTime);

    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeInfo);
    ESP_LOGI(TAG, "Current Date and Time: %s", buffer);

    return CBR_CLOSE_OK;
}

esp_err_t cloud_set_time(void)
{
    http_parser_t http_parser_handler_ctx = {
        .http_data_cbr = &cloud_set_time_cbr,
        .header_buffer = malloc(MAX_HTTP_HEADER_SIZE + 1)};
    cloud_req_t req = {
        .host = CLOUD_HOST,
        .port = CLOUD_PORT,
        .path = "/v1/time",
        .data_received_cbr = &http_parser_received_cbr,
        .data_received_ctx = &http_parser_handler_ctx};

    esp_err_t ret = cloud_request(&req);

    free(http_parser_handler_ctx.header_buffer);

    return ret;
}

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
    if (!req->wbuf)
    {
        req->wbuf = malloc(CLOUD_WRITE_BLOCK);
        if (!req->wbuf)
        {
            ESP_LOGE(TAG, "[CDL] No memory for the write buffer");
            return ESP_FAIL;
        }
    }
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

    http_parser_t http_parser_handler_ctx = {
        .http_status_cbr = &cloud_content_status_cbr,
        .http_data_cbr = &cloud_content_cbr,
        .http_end_cbr = &cloud_content_end_cbr,
        .content_length_cbr = &cloud_content_length_cbr,
        .ctx = content_req,
        .header_buffer = malloc(MAX_HTTP_HEADER_SIZE + 1)};

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

    free(http_parser_handler_ctx.header_buffer);

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

/********************************************************/
/* small POST requests with the response in memory      */
/********************************************************/

typedef struct
{
    uint8_t *buf;
    size_t size;
    size_t len;
    int status;
    bool overflow;
} cloud_mem_ctx_t;

static esp_err_t cloud_mem_status_cbr(void *ctx, int status_code)
{
    cloud_mem_ctx_t *m = (cloud_mem_ctx_t *)ctx;
    m->status = status_code;
    return status_code == 200 ? ESP_OK : ESP_FAIL;
}

static esp_err_t cloud_mem_data_cbr(void *ctx, uint8_t *data, size_t length)
{
    cloud_mem_ctx_t *m = (cloud_mem_ctx_t *)ctx;
    if (m->len + length > m->size)
    {
        m->overflow = true;
        return ESP_FAIL;
    }
    memcpy(m->buf + m->len, data, length);
    m->len += length;
    return ESP_OK;
}

esp_err_t cloud_post(const char *path, const uint8_t *body, size_t body_len, uint8_t *resp, size_t resp_size, size_t *resp_len)
{
    cloud_mem_ctx_t mem = {.buf = resp, .size = resp_size};
    http_parser_t parser = {
        .http_status_cbr = &cloud_mem_status_cbr,
        .http_data_cbr = &cloud_mem_data_cbr,
        .ctx = &mem,
        .header_buffer = malloc(MAX_HTTP_HEADER_SIZE + 1)};
    if (!parser.header_buffer)
    {
        return ESP_ERR_NO_MEM;
    }

    cloud_req_t req = {
        .host = CLOUD_HOST,
        .port = CLOUD_PORT,
        .path = path,
        .body = body,
        .body_len = body_len,
        .data_received_cbr = &http_parser_received_cbr,
        .data_received_ctx = &parser,
        .connection_closed_cbr = &http_parser_closed_cbr,
        .connection_closed_ctx = &parser};

    esp_err_t ret = cloud_request(&req);
    bool complete = parser.finished && parser.has_content_length && parser.received_data_length == parser.content_length;
    free(parser.header_buffer);

    if (ret != ESP_OK || mem.status != 200 || mem.overflow || !complete)
    {
        ESP_LOGE(TAG, "POST %s failed: HTTP %d%s", path, mem.status, mem.overflow ? ", response too large" : "");
        return ESP_FAIL;
    }
    *resp_len = mem.len;
    return ESP_OK;
}

/********************************************************/
/* claim: "this tonie was placed", like the original box */
/********************************************************/

/* teddyCloud (handleCloudClaim) remembers it as the last tonie of the box,
   sends an MQTT event and marks the tonie as claimed. Only to teddyCloud,
   never to the Boxine fallback. A few seconds after placing, so the TLS
   handshake does not compete with the playback start for RAM. */
#define CLAIM_DELAY_MS 5000
#define CLAIM_GIVE_UP_MS 60000
static portMUX_TYPE claim_lock = portMUX_INITIALIZER_UNLOCKED;
static struct
{
    bool pending;
    uint64_t uid;
    uint8_t token[32];
    TickType_t since;
} claim;

void cloud_claim(uint64_t uid, const uint8_t token[32])
{
    if (!settings_get()->claim || !uid || !token)
    {
        return;
    }
    taskENTER_CRITICAL(&claim_lock);
    claim.pending = true;
    claim.uid = uid;
    memcpy(claim.token, token, 32);
    claim.since = xTaskGetTickCount();
    taskEXIT_CRITICAL(&claim_lock);
}

/* "/v1/claim/" + rUID, the same 16 hex digits as in /v2/content/ */
void cloud_claim_path(uint64_t uid, char *path, size_t size)
{
    int n = snprintf(path, size, "/v1/claim/");
    for (int i = 0; i < 8 && n < (int)size; i++)
    {
        n += snprintf(path + n, size - n, "%02X", (unsigned)(uid >> (i * 8)) & 0xFF);
    }
}

static esp_err_t cloud_claim_status_cbr(void *ctx, int status_code)
{
    *(int *)ctx = status_code;
    return ESP_OK;
}

static esp_err_t cloud_ignore_data_cbr(void *ctx, uint8_t *data, size_t length)
{
    return ESP_OK;
}

static void cloud_claim_send(void)
{
    uint64_t uid;
    uint8_t token[32];
    taskENTER_CRITICAL(&claim_lock);
    uid = claim.uid;
    memcpy(token, claim.token, 32);
    claim.pending = false;
    taskEXIT_CRITICAL(&claim_lock);

    char path[32];
    cloud_claim_path(uid, path, sizeof(path));
    char auth[65];
    for (int i = 0; i < 32; i++)
    {
        sprintf(auth + 2 * i, "%02X", token[i]);
    }
    int status = 0;
    http_parser_t parser = {
        .http_status_cbr = &cloud_claim_status_cbr,
        .http_data_cbr = &cloud_ignore_data_cbr,
        .ctx = &status,
        .header_buffer = malloc(MAX_HTTP_HEADER_SIZE + 1)};
    if (!parser.header_buffer)
    {
        return;
    }
    cloud_req_t req = {
        .host = CLOUD_HOST,
        .port = CLOUD_PORT,
        .path = path,
        .auth = auth,
        .data_received_cbr = &http_parser_received_cbr,
        .data_received_ctx = &parser,
        .connection_closed_cbr = &http_parser_closed_cbr,
        .connection_closed_ctx = &parser};
    cloud_request(&req);
    free(parser.header_buffer);
    memset(auth, 0, sizeof(auth));
    memset(token, 0, sizeof(token));
    if (status == 200 || status == 204)
    {
        ESP_LOGI(TAG, "Claim %016llX sent", (unsigned long long)uid);
    }
    else
    {
        ESP_LOGW(TAG, "Claim %016llX: HTTP %d", (unsigned long long)uid, status);
    }
}

/********************************************************/
/* cloud request code, called from external code        */
/********************************************************/

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
    free(req->wbuf);
    vSemaphoreDelete(req->update_sem);
    free(req->filename);
    free(req);
}

/********************************************************/
/* certificate loading                                  */
/********************************************************/

esp_err_t cloud_load_cert(const char *path, uint8_t **ptr, size_t *length)
{
    struct stat st;
    if (stat(path, &st) != 0)
    {
        ESP_LOGE(TAG, "Certificate %s not found", path);
        return ESP_FAIL;
    }

    FILE *ca = fopen(path, "rb");
    if (!ca)
    {
        ESP_LOGE(TAG, "Certificate %s not found", path);
        return ESP_FAIL;
    }

    *ptr = malloc(st.st_size);
    *length = st.st_size;
    if (!*ptr || fread(*ptr, *length, 1, ca) != 1)
    {
        ESP_LOGE(TAG, "Failed to read certificate");
        fclose(ca);
        free(*ptr);
        *ptr = NULL;
        *length = 0;
        return ESP_FAIL;
    }
    fclose(ca);
    ESP_LOGI(TAG, "Loaded '%s' with %d bytes", path, *length);

    return ESP_OK;
}

static void cloud_fail_request(cloud_content_req_t *req, const char *reason)
{
    ESP_LOGW(TAG, "[CDL] Not downloading '%s': %s", req->location, reason);
    req->state = CC_STATE_ERROR;
    xSemaphoreGive(req->update_sem);
}

static void cloud_task(void *ctx)
{
    bool cloud_available = false;
    TickType_t next_try = 0;
    bool fc_done = false;
    TickType_t fc_last = 0;
    bool fc_played = false;
    TickType_t backend_since = 0;
    cloud_taskwatch = taskwatch_add("cloud", 120000);
    while (true)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        taskwatch_feed(cloud_taskwatch);

        /* on the fallback: try to get back to teddyCloud from time to time */
        if (cloud_available && active_backend != BACKEND_TEDDYCLOUD && backends[BACKEND_TEDDYCLOUD].host &&
            xTaskGetTickCount() - backend_since >= pdMS_TO_TICKS(BACKEND_RETRY_PREFERRED_MS))
        {
            cloud_available = false;
            next_try = 0;
        }

        if (wifi_is_connected() && !cloud_available && (int32_t)(xTaskGetTickCount() - next_try) >= 0)
        {
            int previous = active_backend;
            for (int b = 0; b < 2 && !cloud_available; b++)
            {
                if (!backends[b].host)
                {
                    continue;
                }
                active_backend = b;
                ESP_LOGI(TAG, "Try to get time from %s (%s:%d)", backends[b].name, CLOUD_HOST, CLOUD_PORT);
                cloud_available = (cloud_set_time() == ESP_OK);
            }
            if (cloud_available)
            {
                backend_since = xTaskGetTickCount();
                if (active_backend == BACKEND_BOXINE)
                {
                    ESP_LOGW(TAG, "Using the Boxine cloud directly (fallback)");
                }
                if (active_backend != previous)
                {
                    /* other backend, other content versions: check freshness there too */
                    fc_done = false;
                }
            }
            else
            {
                active_backend = previous;
            }
            next_try = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
        }

        teddycloud_up = cloud_available && active_backend == BACKEND_TEDDYCLOUD;
        if (teddycloud_up)
        {
            /* names of the tonies from teddyCloud's web API (plain HTTP, short) */
            tonie_names_tc_step();
        }

        /* freshness check after boot, then every freshness_interval minutes
           and after a tonie was removed (at most every 15 minutes), between
           downloads; not while something plays, a TLS handshake next to the
           decoder is the tightest spot for the internal RAM */
        bool playing = pb_is_playing();
        if (playing)
        {
            fc_played = true;
        }
        TickType_t fc_age = xTaskGetTickCount() - fc_last;
        TickType_t fc_interval = pdMS_TO_TICKS((uint32_t)settings_get()->freshness_min * FRESHNESS_MINUTE_MS);
        bool after_play = fc_played && fc_age >= pdMS_TO_TICKS(FRESHNESS_AFTER_PLAY_MIN * FRESHNESS_MINUTE_MS);
        if (cloud_available && !playing && (!fc_done || fc_age >= fc_interval || after_play))
        {
            fc_played = false;
            fc_last = xTaskGetTickCount();
            fc_done = freshness_run() == ESP_OK;
            if (!fc_done)
            {
                /* retry in a minute, not in a loop */
                fc_done = true;
                fc_last = xTaskGetTickCount() - fc_interval + pdMS_TO_TICKS(FRESHNESS_MINUTE_MS);
            }
        }

        if (claim.pending)
        {
            TickType_t age = xTaskGetTickCount() - claim.since;
            if (age >= pdMS_TO_TICKS(CLAIM_GIVE_UP_MS) || active_backend != BACKEND_TEDDYCLOUD)
            {
                claim.pending = false;
            }
            else if (cloud_available && wifi_is_connected() && age >= pdMS_TO_TICKS(CLAIM_DELAY_MS))
            {
                cloud_claim_send();
            }
        }

        cloud_content_req_t *req;
        if (xQueueReceive(cloud_request_queue, &req, 0) != pdTRUE)
        {
            continue;
        }

        /* every request gets an answer, otherwise the playback side would wait forever */
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
        /* last access to req, the requester frees it after this */
        req->done = true;

        if (connection_failed)
        {
            /* check reachability again before the next request */
            cloud_available = false;
        }
    }
    vTaskDelete(NULL);
}

static void cloud_init_user_agent(void)
{
    esp_app_desc_t ofw;
    if (settings_get()->user_agent_original && bootsel_get_original_desc(&ofw))
    {
        /* same format as the original firmware: "<project>/<version>" */
        snprintf(user_agent, sizeof(user_agent), "%s/%s", ofw.project_name, ofw.version);
        ESP_LOGW(TAG, "Sending the original firmware's user agent '%s'", user_agent);
    }
    else
    {
        if (settings_get()->user_agent_original)
        {
            ESP_LOGW(TAG, "No original firmware found, using the TeddyBox user agent");
        }
        snprintf(user_agent, sizeof(user_agent), "toniebox-esp32-teddybox/%s", running_app_info.version);
    }
}

void cloud_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    esp_ota_get_partition_description(esp_ota_get_running_partition(), &running_app_info);
    cloud_init_user_agent();

    const settings_t *s = settings_get();
    if (s->cloud_enabled)
    {
        backends[BACKEND_TEDDYCLOUD] = (cloud_backend_t){.name = "teddyCloud", .host = s->cloud_host, .port = s->cloud_port};
        if (cloud_load_cert(s->ca_path, &backends[BACKEND_TEDDYCLOUD].ca, &backends[BACKEND_TEDDYCLOUD].ca_len) != ESP_OK)
        {
            cloud_load_cert("/spiflash/cert/ca.der", &backends[BACKEND_TEDDYCLOUD].ca, &backends[BACKEND_TEDDYCLOUD].ca_len);
        }
    }
    if (s->boxine_fallback && s->boxine_host[0])
    {
        /* the CA in flash is the Boxine CA unless the box was patched for teddyCloud */
        /* SNI and host name check like the original firmware */
        backends[BACKEND_BOXINE] = (cloud_backend_t){.name = "Boxine", .host = s->boxine_host, .port = 443, .tls_name = s->boxine_host};
        if (cloud_load_cert("/spiflash/cert/ca.der", &backends[BACKEND_BOXINE].ca, &backends[BACKEND_BOXINE].ca_len) != ESP_OK)
        {
            backends[BACKEND_BOXINE].host = NULL;
        }
    }

    if (!backends[BACKEND_TEDDYCLOUD].host && !backends[BACKEND_BOXINE].host)
    {
        ESP_LOGI(TAG, "No cloud configured, playing local content only");
        return;
    }
    active_backend = backends[BACKEND_TEDDYCLOUD].host ? BACKEND_TEDDYCLOUD : BACKEND_BOXINE;

    cloud_request_queue = xQueueCreate(4, sizeof(cloud_content_req_t *));

    ESP_LOGI(TAG, "Cloud: teddyCloud %s, Boxine fallback %s, user agent '%s'",
             backends[BACKEND_TEDDYCLOUD].host ? backends[BACKEND_TEDDYCLOUD].host : "-",
             backends[BACKEND_BOXINE].host ? backends[BACKEND_BOXINE].host : "off", user_agent);
    cloud_load_cert("/spiflash/cert/client.der", &client_der, &client_der_len);
    cloud_load_cert("/spiflash/cert/private.der", &private_der, &private_der_len);

    xTaskCreate(&cloud_task, "[TB] cloud", 5000, NULL, 5, NULL);
}
