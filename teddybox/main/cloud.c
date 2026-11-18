
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
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

#include "cloud.h"
#include "wifi.h"
#include "playback.h"
#include "settings.h"
#include "freshness.h"

#define CLOUD_HOST (settings_get()->cloud_host)
#define CLOUD_PORT (settings_get()->cloud_port)

static const char *TAG = "Cloud";

uint8_t *ca_der = NULL;
uint8_t *client_der = NULL;
uint8_t *private_der = NULL;
size_t ca_der_len = 0;
size_t client_der_len = 0;
size_t private_der_len = 0;
static esp_app_desc_t running_app_info;
static QueueHandle_t cloud_request_queue;

/********************************************************/
/* TLS connection handling                              */
/********************************************************/

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
        asprintf(&range_line, "Range: bytes=%d-\r\n", req->range_start);
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
                       "User-Agent: toniebox-esp32-teddybox/%s\r\n"
                       "Connection: close\r\n"
                       "\r\n",
             req->body ? "POST" : "GET", req->path, req->host, auth_line, range_line, body_line, running_app_info.version);

    // ESP_LOGI(TAG, "Header %s...", request);
    esp_tls_cfg_t cfg = {
        .cacert_buf = ca_der,
        .cacert_bytes = ca_der_len,
        .clientcert_buf = client_der,
        .clientcert_bytes = client_der_len,
        .clientkey_buf = private_der,
        .clientkey_bytes = private_der_len,
        .timeout_ms = 10000,
        .skip_common_name = true};

    uint8_t *receive_buffer = malloc(HTTP_RECEIVE_SIZE);
    if (!receive_buffer)
    {
        ESP_LOGE(TAG, "Allocation failed...");
        return ESP_FAIL;
    }

    struct esp_tls *tls = esp_tls_conn_http_new(url, &cfg);
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

    ret = ESP_OK;

    while (connected)
    {
        int len = esp_tls_conn_read(tls, receive_buffer, HTTP_RECEIVE_SIZE);

        if (len == ESP_TLS_ERR_SSL_WANT_WRITE || len == ESP_TLS_ERR_SSL_WANT_READ)
        {
            ESP_LOGI(TAG, "TLS socket idle");
            continue;
        }

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
    if (req->connection_closed_cbr)
    {
        req->connection_closed_cbr(req->connection_closed_ctx);
    }

exit:
    esp_tls_conn_delete(tls);

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

    /* the server may ignore our Range request and send everything again */
    bool resume = range_start > 0 && range_start == req->received;
    req->received = range_start;
    req->content_length = total_length;

    ESP_LOGI(TAG, "[CDL] download %u-%u to '%s'%s", req->received, req->content_length, req->filename, resume ? " (resume)" : "");

    cloud_create_directories(req->filename);
    /* "wb+" would truncate the part we already have */
    req->handle = fopen(req->filename, resume ? "rb+" : "wb+");
    if (!req->handle)
    {
        ESP_LOGE(TAG, "[CDL] Failed to open '%s'", req->filename);
        return ESP_FAIL;
    }

    req->state = CC_STATE_RECEIVING;

    return ESP_OK;
}

esp_err_t cloud_content_cbr(void *ctx, uint8_t *data, size_t length)
{
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

    while (!xSemaphoreTake(req->file_sem, 1000 / portTICK_PERIOD_MS))
    {
        ESP_LOGE(TAG, "[CDL] Timed out waiting for file lock...");
    }
    fseek(req->handle, req->received, SEEK_SET);

    req->received += length;
    if (fwrite(data, length, 1, req->handle) != 1)
    {
        xSemaphoreGive(req->file_sem);
        ESP_LOGE(TAG, "[CDL] Write failed, bailing out");
        return ESP_FAIL;
    }
    xSemaphoreGive(req->file_sem);
    xSemaphoreGive(req->update_sem);

    /* give others the chance to react */
    vTaskDelay(1 / portTICK_RATE_MS);

    time_t current = time(NULL);
    static time_t start = 0;
    static time_t last = 0;
    if (start == 0)
    {
        start = current;
    }

    if (last != current)
    {
        last = current;

        float elapsed = difftime(current, start);
        float speed = (req->received / elapsed) / 1024.0; // KiB/s
        float percent_complete = (req->received * 100.0f / req->content_length);
        float eta = ((req->content_length - req->received) / (req->received / elapsed));

        ESP_LOGI(TAG, "[CDL] %d bytes received (%2.2f%%), Speed: %2.2f KiB/s, ETA: %2.2f s", req->received, percent_complete, speed, eta);
    }
    return ESP_OK;
}

esp_err_t cloud_content_end_cbr(void *ctx)
{
    cloud_content_req_t *req = (cloud_content_req_t *)ctx;
    ESP_LOGI(TAG, "[CDL] End transfer, %d/%d received", req->received, req->content_length);

/* playback handler initiated the download, it shall close handles
    if (req->handle)
    {
        fclose(req->handle);
        req->handle = NULL;
    }
*/
    if (req->state == CC_STATE_RECEIVING && req->received >= req->content_length)
    {
        req->state = CC_STATE_FINISHED;
    }
    else
    {
        /* aborted, HTTP error (state already ERROR) or connection lost midway */
        req->state = CC_STATE_ERROR;
    }
    xSemaphoreGive(req->update_sem);
    return ESP_OK;
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
    if (content_req->state != CC_STATE_FINISHED && content_req->state != CC_STATE_ERROR)
    {
        ESP_LOGE(TAG, "[CDL] Request for '%s' failed", content_req->location);
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
    req->auth = malloc(65);

    /* when the file already exists, do a partial download */
    struct stat st;
    if (stat(req->filename, &st) == 0)
    {
        req->received = st.st_size;
        ESP_LOGI(TAG, "[CDL] Partial file, continue at %d", req->received);
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
    while (true)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);

        if (wifi_is_connected() && !cloud_available && (int32_t)(xTaskGetTickCount() - next_try) >= 0)
        {
            ESP_LOGI(TAG, "Try to get time from %s:%d", CLOUD_HOST, CLOUD_PORT);
            cloud_available = (cloud_set_time() == ESP_OK);
            next_try = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
        }

        /* freshness check after boot and then every few hours, between downloads */
        if (cloud_available && (!fc_done || xTaskGetTickCount() - fc_last >= pdMS_TO_TICKS(FRESHNESS_INTERVAL_MS)))
        {
            fc_last = xTaskGetTickCount();
            fc_done = freshness_run() == ESP_OK;
            if (!fc_done)
            {
                /* retry in a minute, not in a loop */
                fc_done = true;
                fc_last = xTaskGetTickCount() - pdMS_TO_TICKS(FRESHNESS_INTERVAL_MS) + pdMS_TO_TICKS(60000);
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

void cloud_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    esp_ota_get_partition_description(esp_ota_get_running_partition(), &running_app_info);

    if (!settings_get()->cloud_enabled)
    {
        ESP_LOGI(TAG, "Cloud disabled in settings, playing local content only");
        return;
    }

    cloud_request_queue = xQueueCreate(4, sizeof(cloud_content_req_t *));

    ESP_LOGI(TAG, "Loading certificates, cloud is %s:%d", CLOUD_HOST, CLOUD_PORT);
    if (cloud_load_cert(settings_get()->ca_path, &ca_der, &ca_der_len) != ESP_OK)
    {
        cloud_load_cert("/spiflash/cert/ca.der", &ca_der, &ca_der_len);
    }
    cloud_load_cert("/spiflash/cert/client.der", &client_der, &client_der_len);
    cloud_load_cert("/spiflash/cert/private.der", &private_der, &private_der_len);

    xTaskCreate(&cloud_task, "[TB] cloud", 5000, NULL, 5, NULL);
}
