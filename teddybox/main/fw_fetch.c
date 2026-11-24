#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "fw_fetch.h"
#include "ota.h"
#include "i18n.h"

static const char *TAG = "FwFetch";

typedef enum
{
    FF_IDLE,
    FF_RUNNING,
    FF_DONE,
    FF_ERROR,
} ff_state_t;

static volatile ff_state_t state = FF_IDLE;
static volatile int received, total;
static volatile bool needs_pd;
static const char *volatile error_text;
static char tag[24];
static bool allow_pd;
static void (*touch_cb)(void);
static void (*finished_cb)(void);

static bool tag_ok(const char *t)
{
    /* v<digits>.<digits>.<digits> */
    if (*t++ != 'v')
    {
        return false;
    }
    for (int part = 0; part < 3; part++)
    {
        if (!isdigit((unsigned char)*t))
        {
            return false;
        }
        while (isdigit((unsigned char)*t))
        {
            t++;
        }
        if (part < 2 && *t++ != '.')
        {
            return false;
        }
    }
    return *t == 0;
}

static esp_http_client_handle_t open_url(const char *url, int *len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096, /* GitHub's redirect has long headers */
        .buffer_size_tx = 1024,
        .max_redirection_count = 5,
        .user_agent = "TeddyBox",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c)
    {
        return NULL;
    }
    /* follow the redirect to GitHub's download host by hand: open, read the
       headers, and on 3xx move on to the Location */
    for (int hop = 0; hop < 5; hop++)
    {
        if (esp_http_client_open(c, 0) != ESP_OK)
        {
            break;
        }
        int64_t l = esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308)
        {
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue;
        }
        if (st != 200)
        {
            ESP_LOGW(TAG, "HTTP %d for %s", st, url);
            break;
        }
        *len = (int)l;
        return c;
    }
    esp_http_client_cleanup(c);
    return NULL;
}

/* SHA-256 of teddybox-<tag>.bin from SHA256SUMS of the release */
static bool fetch_sum(const char *base, uint8_t sum[32])
{
    char url[160], line[160];
    snprintf(url, sizeof(url), "%s/SHA256SUMS", base);
    int len = 0;
    esp_http_client_handle_t c = open_url(url, &len);
    if (!c)
    {
        return false;
    }
    char *buf = calloc(1, 2049);
    bool found = false;
    if (buf)
    {
        int n = 0, r;
        while (n < 2048 && (r = esp_http_client_read(c, buf + n, 2048 - n)) > 0)
        {
            n += r;
        }
        char want[48];
        snprintf(want, sizeof(want), "teddybox-%s.bin", tag);
        for (char *p = strtok(buf, "\n"); p && !found; p = strtok(NULL, "\n"))
        {
            snprintf(line, sizeof(line), "%s", p);
            char *name = strchr(line, ' ');
            if (!name || name - line != 64)
            {
                continue;
            }
            while (*name == ' ' || *name == '*')
            {
                name++;
            }
            name[strcspn(name, "\r")] = 0;
            if (strcmp(name, want))
            {
                continue;
            }
            found = true;
            for (int i = 0; i < 32; i++)
            {
                unsigned v;
                if (sscanf(line + 2 * i, "%2x", &v) != 1)
                {
                    found = false;
                }
                sum[i] = v;
            }
        }
        free(buf);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return found;
}

static void fetch_task(void *arg)
{
    char base[120], url[160];
    snprintf(base, sizeof(base), "https://github.com/%s/releases/download/%s", TEDDYBOX_REPO, tag);
    snprintf(url, sizeof(url), "%s/teddybox-%s.bin", base, tag);
    ESP_LOGI(TAG, "Online update to %s", tag);

    const char *err = NULL;
    uint8_t want[32], got_sum[32];
    esp_http_client_handle_t c = NULL;
    uint8_t *buf = NULL;
    ota_session_t ota;
    bool begun = false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    if (!fetch_sum(base, want))
    {
        err = L("GitHub not reachable or the release has no SHA256SUMS", "GitHub nicht erreichbar oder die Version hat keine SHA256SUMS");
        goto out;
    }
    int len = 0;
    c = open_url(url, &len);
    if (!c || len <= 0)
    {
        err = L("Firmware file of this release not found", "Firmware-Datei dieser Version nicht gefunden");
        goto out;
    }
    total = len;
    buf = malloc(4096);
    if (!buf)
    {
        err = L("Out of memory", "Kein Speicher frei");
        goto out;
    }
    int fill = 0;
    while (received < total)
    {
        int r = esp_http_client_read(c, (char *)buf + fill, 4096 - fill);
        if (r <= 0)
        {
            err = L("Download interrupted", "Download abgebrochen");
            break;
        }
        fill += r;
        if (fill < 4096 && received + fill < total)
        {
            continue;
        }
        mbedtls_sha256_update(&sha, buf, fill);
        if (!begun)
        {
            esp_app_desc_t desc;
            bool pd = false;
            err = ota_check_header(buf, fill, total, &desc);
            if (!err)
            {
                err = ota_session_begin(&ota, total, allow_pd, &pd);
                needs_pd = pd;
            }
            if (err)
            {
                break;
            }
            begun = true;
        }
        err = ota_session_write(&ota, buf, fill);
        if (err)
        {
            break;
        }
        received += fill;
        fill = 0;
        if (touch_cb)
        {
            touch_cb();
        }
    }
    if (!err)
    {
        mbedtls_sha256_finish(&sha, got_sum);
        if (memcmp(want, got_sum, 32))
        {
            err = L("Checksum does not match SHA256SUMS", "Prüfsumme passt nicht zu SHA256SUMS");
        }
    }
    if (!err && begun)
    {
        err = ota_session_finish(&ota);
    }
    else if (begun)
    {
        ota_session_abort(&ota);
    }

out:
    mbedtls_sha256_free(&sha);
    free(buf);
    if (c)
    {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    if (err)
    {
        ESP_LOGE(TAG, "Online update failed: %s", err);
        error_text = err;
        state = FF_ERROR;
    }
    else
    {
        ESP_LOGI(TAG, "Online update to %s installed", tag);
        state = FF_DONE;
        vTaskDelay(pdMS_TO_TICKS(3000)); /* the page shows it first */
        if (finished_cb)
        {
            finished_cb();
        }
    }
    vTaskDelete(NULL);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return e;
}

static esp_err_t start_handler(httpd_req_t *req)
{
    if (touch_cb)
    {
        touch_cb();
    }
    char q[64] = "", t[sizeof(tag)] = "", pd[4] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "tag", t, sizeof(t));
    httpd_query_key_value(q, "pd", pd, sizeof(pd));
    cJSON *j = cJSON_CreateObject();
    if (state == FF_RUNNING || state == FF_DONE)
    {
        cJSON_AddBoolToObject(j, "ok", false);
        cJSON_AddStringToObject(j, "error", L("An update is already running", "Ein Update läuft schon"));
    }
    else if (!tag_ok(t))
    {
        cJSON_AddBoolToObject(j, "ok", false);
        cJSON_AddStringToObject(j, "error", L("Invalid version", "Ungültige Version"));
    }
    else
    {
        snprintf(tag, sizeof(tag), "%s", t);
        allow_pd = pd[0] == '1';
        received = total = 0;
        needs_pd = false;
        error_text = NULL;
        state = FF_RUNNING;
        if (xTaskCreate(fetch_task, "[TB] fw fetch", 8192, NULL, 5, NULL) != pdPASS)
        {
            state = FF_ERROR;
            error_text = L("Out of memory", "Kein Speicher frei");
        }
        cJSON_AddBoolToObject(j, "ok", state == FF_RUNNING);
    }
    return send_json(req, j);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (touch_cb)
    {
        touch_cb();
    }
    static const char *const names[] = {"idle", "running", "done", "error"};
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", names[state]);
    cJSON_AddNumberToObject(j, "received", received);
    cJSON_AddNumberToObject(j, "total", total);
    cJSON_AddStringToObject(j, "tag", tag);
    if (state == FF_ERROR)
    {
        cJSON_AddStringToObject(j, "error", error_text ? error_text : "?");
        cJSON_AddBoolToObject(j, "needs_pd", needs_pd);
    }
    return send_json(req, j);
}

esp_err_t fw_fetch_register(httpd_handle_t server, void (*touch)(void), void (*finished)(void))
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/firmware/fetch", .method = HTTP_POST, .handler = start_handler},
        {.uri = "/api/firmware/fetch/status", .method = HTTP_GET, .handler = status_handler},
    };
    touch_cb = touch;
    finished_cb = finished;
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
