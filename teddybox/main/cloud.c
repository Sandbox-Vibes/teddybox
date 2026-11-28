/*
 * Cloud task and connections: picks the backend (teddyCloud, or the Boxine
 * cloud as opt-in fallback), keeps the time, runs the freshness check and
 * the names lookup, and serves the requests of the other parts one after
 * the other. Content downloads: cloud_download.c, "tonie placed" (claim):
 * cloud_claim.c. Shared internals: cloud_internal.h.
 */

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
#include "mbedtls/x509_crt.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "memdiag.h"
#include "mempeak.h"
#include "cloud.h"
#include "maint.h"
#include "wifi.h"
#include "playback.h"
#include "settings.h"
#include "freshness.h"
#include "tonie_names_tc.h"
#include "taskwatch.h"
#include "bootsel.h"
#include "sd_retry.h"
#include "cloud_internal.h"
#include "taf_verify.h"
#include "rtnl.h"

cloud_backend_t backends[2];
volatile int active_backend = BACKEND_TEDDYCLOUD;
/* teddyCloud answers (time sync worked) and is the backend in use */
static volatile bool teddycloud_up = false;
/* WiFi up and a backend answered (time request): downloads can start */
static volatile bool cloud_ready = false;
static char user_agent[64];

static const char *TAG = "Cloud";

uint8_t *client_der = NULL;
uint8_t *private_der = NULL;
size_t client_der_len = 0;
size_t private_der_len = 0;
static esp_app_desc_t running_app_info;
QueueHandle_t cloud_request_queue;

/********************************************************/
/* TLS connection handling                              */
/********************************************************/

static cloud_bufs_t *bufs;

cloud_bufs_t *cloud_bufs(void)
{
    if (!bufs)
    {
        bufs = heap_caps_malloc(sizeof(*bufs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!bufs)
        {
            ESP_LOGE(TAG, "No memory for the request buffers (%u bytes)", (unsigned)sizeof(*bufs));
        }
    }
    return bufs;
}

uint8_t *cloud_header_buffer(void)
{
    cloud_bufs_t *b = cloud_bufs();
    return b ? b->header : NULL;
}

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
int cloud_taskwatch = -1;

/* a TLS request of the cloud task is running (RTNL steps aside, see
   cloud_tls_busy) */
static int request_active = 0;

static esp_err_t cloud_request_do(cloud_req_t *req);

esp_err_t cloud_request(cloud_req_t *req)
{
    __atomic_add_fetch(&request_active, 1, __ATOMIC_SEQ_CST);
    /* let RTNL close its connection first (it checks every 100 ms): 28 Nov,
       the freshness handshake started while RTNL was still connected and
       free RAM fell to 2.7 KB */
    for (int i = 0; i < 20 && rtnl_is_connected(); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_err_t ret = cloud_request_do(req);
    __atomic_sub_fetch(&request_active, 1, __ATOMIC_SEQ_CST);
    return ret;
}

bool cloud_tls_busy(void)
{
    return cloud_download_busy() || __atomic_load_n(&request_active, __ATOMIC_SEQ_CST) > 0;
}

static esp_err_t cloud_request_do(cloud_req_t *req)
{
    esp_err_t ret = ESP_FAIL;
    cloud_bufs_t *b = cloud_bufs();
    if (!b)
    {
        return ESP_FAIL;
    }

    char auth_line[96] = "";
    char range_line[40] = "";
    char *request = b->request;
    char *url = b->url;

    if (req->auth)
    {
        snprintf(auth_line, sizeof(auth_line), "Authorization: BD %s\r\n", req->auth);
    }
    if (req->range_start)
    {
        snprintf(range_line, sizeof(range_line), "Range: bytes=%" PRIu32 "-\r\n", req->range_start);
    }
    if (snprintf(url, sizeof(b->url), "https://%s:%d%s", req->host, req->port, req->path) >= (int)sizeof(b->url))
    {
        ESP_LOGE(TAG, "URL too long: %s", req->path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Connect to %s...", url);

    char body_line[48] = "";
    if (req->body)
    {
        snprintf(body_line, sizeof(body_line), "Content-Length: %u\r\n", (unsigned)req->body_len);
    }

    int request_n = snprintf(request, sizeof(b->request), "%s %s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "%s"
                       "%s"
                       "%s"
                       "User-Agent: %s\r\n"
                       "Connection: close\r\n"
                       "\r\n",
             req->body ? "POST" : "GET", req->path, req->host, auth_line, range_line, body_line, user_agent);
    if (request_n >= (int)sizeof(b->request))
    {
        ESP_LOGE(TAG, "Request too long: %s", req->path);
        return ESP_FAIL;
    }

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

    uint8_t *receive_buffer = b->rx;

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
    /* the request may carry the auth token */
    memset(b->request, 0, sizeof(b->request));
    memset(auth_line, 0, sizeof(auth_line));

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

    maint_clock_synced(unixTime);
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
        .header_buffer = cloud_header_buffer()};
    if (!http_parser_handler_ctx.header_buffer)
    {
        return ESP_ERR_NO_MEM;
    }
    cloud_req_t req = {
        .host = CLOUD_HOST,
        .port = CLOUD_PORT,
        .path = "/v1/time",
        .data_received_cbr = &http_parser_received_cbr,
        .data_received_ctx = &http_parser_handler_ctx};

    esp_err_t ret = cloud_request(&req);


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
        .header_buffer = cloud_header_buffer()};
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

bool cloud_is_enabled(void)
{
    return cloud_request_queue != NULL;
}

bool cloud_backend_is_boxine(void)
{
    return active_backend == BACKEND_BOXINE;
}

bool cloud_is_ready(void)
{
    return cloud_ready && wifi_is_connected();
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
        cloud_ready = cloud_available && wifi_is_connected();
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
        /* and not while a file is checked in the background (its buffers and
           stack): 28 Nov, end of a tonie: check of the new download, RTNL
           connection and this handshake together left 1.4 KB, WiFi could
           not allocate a buffer */
        if (cloud_available && !playing && !taf_verify_running() &&
            (!fc_done || fc_age >= fc_interval || after_play))
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

        cloud_claim_step(cloud_available);

        cloud_content_req_t *req;
        if (xQueueReceive(cloud_request_queue, &req, 0) != pdTRUE)
        {
            continue;
        }

        /* every request gets an answer, otherwise the playback side would wait forever */
        bool connection_failed = cloud_download_serve(req, cloud_available);

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

bool cloud_ca_is_teddycloud(const uint8_t *der, size_t len)
{
    /* teddyCloud names its own root "TeddyCloud Root CA" (src/cert.c) */
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    bool tc = false;
    if (mbedtls_x509_crt_parse_der(&crt, der, len) == 0)
    {
        char subject[128];
        if (mbedtls_x509_dn_gets(subject, sizeof(subject), &crt.subject) > 0)
        {
            tc = strstr(subject, "TeddyCloud") != NULL;
        }
    }
    mbedtls_x509_crt_free(&crt);
    return tc;
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
        /* SNI and host name check like the original firmware */
        cloud_backend_t *bx = &backends[BACKEND_BOXINE];
        *bx = (cloud_backend_t){.name = "Boxine", .host = s->boxine_host, .port = 443, .tls_name = s->boxine_host};
        const char *ca = s->boxine_ca[0] ? s->boxine_ca : "/spiflash/cert/ca.der";
        const cloud_backend_t *tc = &backends[BACKEND_TEDDYCLOUD];
        if (cloud_load_cert(ca, &bx->ca, &bx->ca_len) != ESP_OK)
        {
            ESP_LOGE(TAG, "Boxine fallback off: no CA in '%s'", ca);
            bx->host = NULL;
        }
        else if (cloud_ca_is_teddycloud(bx->ca, bx->ca_len) ||
                 (tc->ca && tc->ca_len == bx->ca_len && !memcmp(tc->ca, bx->ca, bx->ca_len)))
        {
            /* the box was patched for teddyCloud: every Boxine connection
               would fail the server check */
            ESP_LOGE(TAG, "Boxine fallback off: '%s' is the CA of teddyCloud, not of the Boxine cloud. "
                          "Put the original one (teddyCloud: certs/client/ca.der) on the SD card, "
                          "setup page Advanced or boxine_ca in teddybox.ini", ca);
            free(bx->ca);
            bx->ca = NULL;
            bx->host = NULL;
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
