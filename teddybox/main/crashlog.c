#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_core_dump.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "crashlog.h"

static const char *TAG = "CRASH";
#define CRASHLOG_FILE "/sdcard/tb_crash.txt"
#define CRASHLOG_NVS "TB_CRASH"

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
