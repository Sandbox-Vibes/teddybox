#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_core_dump.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "crashlog.h"

static const char *TAG = "CRASH";
#define CRASHLOG_FILE "/sdcard/tb_crash.txt"
#define CRASHLOG_NVS "TB_CRASH"
#define CRASHLOG_VIEW_MAX 8192

void crashlog_check(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    if (esp_core_dump_image_check() != ESP_OK)
    {
        return; /* no (valid) dump */
    }
    esp_core_dump_summary_t *s = calloc(1, sizeof(*s));
    if (!s)
    {
        return;
    }
    if (esp_core_dump_get_summary(s) != ESP_OK)
    {
        ESP_LOGW(TAG, "Core dump present, summary not readable (other firmware?)");
        free(s);
        return;
    }
    char own[APP_ELF_SHA256_SZ] = "";
    esp_app_get_elf_sha256(own, sizeof(own));
    bool same_app = !strncmp((const char *)s->app_elf_sha256, own, 8);

    char line[300];
    int n = snprintf(line, sizeof(line), "task %.16s, PC 0x%08lx, cause %lu, addr 0x%08lx, backtrace", s->exc_task,
                     (unsigned long)s->exc_pc, (unsigned long)s->ex_info.exc_cause, (unsigned long)s->ex_info.exc_vaddr);
    for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16 && n < (int)sizeof(line) - 12; i++)
    {
        n += snprintf(line + n, sizeof(line) - n, " 0x%08lx", (unsigned long)s->exc_bt_info.bt[i]);
    }
    if (s->exc_bt_info.corrupted && n < (int)sizeof(line) - 12)
    {
        snprintf(line + n, sizeof(line) - n, " (corrupted)");
    }
    ESP_LOGW(TAG, "Last crash (%s firmware %.8s): %s", same_app ? "this" : "other", (const char *)s->app_elf_sha256,
             line);

    /* once per crash into a file on the SD card */
    uint32_t id = s->exc_pc ^ (s->exc_bt_info.depth ? s->exc_bt_info.bt[s->exc_bt_info.depth - 1] : 0) ^
                  (uint32_t)strtoul((const char *)s->app_elf_sha256 + 0, NULL, 16);
    for (const char *p = s->exc_task; *p; p++)
    {
        id = id * 31 + (uint8_t)*p;
    }
    nvs_handle_t h;
    uint32_t last = 0;
    if (nvs_open(CRASHLOG_NVS, NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_get_u32(h, "last", &last);
        if (last != id)
        {
            FILE *f = fopen(CRASHLOG_FILE, "a");
            if (f)
            {
                const esp_app_desc_t *app = esp_app_get_description();
                fprintf(f, "TeddyBox %s (running now), crash in firmware ELF %.8s%s:\n  %s\n", app->version,
                        (const char *)s->app_elf_sha256, same_app ? " (this one)" : "", line);
                fclose(f);
                ESP_LOGI(TAG, "Written to %s", CRASHLOG_FILE);
            }
            nvs_set_u32(h, "last", id);
            nvs_commit(h);
        }
        nvs_close(h);
    }
    free(s);
}

/* setup page, tab System: show and delete tb_crash.txt */
static void (*crash_touch)(void);

static esp_err_t crashlog_get_handler(httpd_req_t *req)
{
    if (crash_touch)
    {
        crash_touch();
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    FILE *f = fopen(CRASHLOG_FILE, "r");
    if (!f)
    {
        return httpd_resp_send(req, NULL, 0);
    }
    /* only the last 8 KB, newest crashes are at the end */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size > CRASHLOG_VIEW_MAX)
    {
        fseek(f, size - CRASHLOG_VIEW_MAX, SEEK_SET);
    }
    else
    {
        fseek(f, 0, SEEK_SET);
    }
    char *buf = malloc(1024);
    if (!buf)
    {
        fclose(f);
        return httpd_resp_send_500(req);
    }
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, 1024, f)) > 0)
    {
        err = httpd_resp_send_chunk(req, buf, n);
    }
    free(buf);
    fclose(f);
    if (err == ESP_OK)
    {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t crashlog_delete_handler(httpd_req_t *req)
{
    if (crash_touch)
    {
        crash_touch();
    }
    bool ok = remove(CRASHLOG_FILE) == 0;
    ESP_LOGI(TAG, "%s deleted from the setup page", CRASHLOG_FILE);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

esp_err_t crashlog_register(httpd_handle_t server, void (*touch)(void))
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/crashlog", .method = HTTP_GET, .handler = crashlog_get_handler},
        {.uri = "/api/crashlog/delete", .method = HTTP_POST, .handler = crashlog_delete_handler},
    };
    crash_touch = touch;
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
