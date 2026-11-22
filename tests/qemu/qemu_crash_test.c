/* QEMU test of crashlog.c: the first start crashes on purpose (NULL write in
   a task "crasher"), ESP-IDF writes the core dump and restarts; the second
   start logs the summary and writes it once to /sdcard/tb_crash.txt;
   a third check does not write it again. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_core_dump.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "crashlog.h"

static const char *TAG = "CTEST";
bool wifi_is_connected(void) { return false; }
static int fails;
static void check(const char *w, bool ok) { fails += !ok; ESP_LOGI(TAG, "%s %s", ok ? "OK  " : "FAIL", w); }
static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? st.st_size : -1; }

static void crasher(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    volatile int *p = NULL;
    *p = 42;
}

void app_main(void)
{
    nvs_flash_init();
    static wl_handle_t wl;
    esp_vfs_fat_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 4};
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl("/sdcard", "assets", &mc, &wl));

    nvs_handle_t h;
    uint8_t stage = 0;
    nvs_open("TB_CTEST", NVS_READWRITE, &h);
    nvs_get_u8(h, "stage", &stage);
    if (stage == 0)
    {
        ESP_LOGI(TAG, "stage 1: crash now");
        esp_core_dump_image_erase();
        nvs_erase_all(h);
        nvs_set_u8(h, "stage", 1);
        nvs_commit(h);
        nvs_close(h);
        nvs_handle_t c;
        if (nvs_open("TB_CRASH", NVS_READWRITE, &c) == ESP_OK)
        {
            nvs_erase_all(c);
            nvs_commit(c);
            nvs_close(c);
        }
        remove("/sdcard/tb_crash.txt");
        xTaskCreate(crasher, "crasher", 2048, NULL, 5, NULL);
        while (1)
        {
            vTaskDelay(1000);
        }
    }
    nvs_set_u8(h, "stage", 0);
    nvs_commit(h);
    nvs_close(h);

    check("dump present after the crash", esp_core_dump_image_check() == ESP_OK);
    crashlog_check();
    long a = fsize("/sdcard/tb_crash.txt");
    FILE *f = fopen("/sdcard/tb_crash.txt", "r");
    char buf[400] = "";
    if (f)
    {
        fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    ESP_LOGI(TAG, "file: %s", buf);
    check("file written with task and backtrace", a > 0 && strstr(buf, "task crasher") && strstr(buf, "backtrace 0x"));
    crashlog_check();
    check("not written twice", fsize("/sdcard/tb_crash.txt") == a);
    ESP_LOGI(TAG, "%s", fails ? "FAILED" : "ALL OK");
    while (1)
    {
        vTaskDelay(1000);
    }
}
