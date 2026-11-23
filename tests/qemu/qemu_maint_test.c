/* QEMU test of the nightly update report (maint.c): a run that did not reach
   maint_run_end() is reported at the next start with the step it was in;
   a run that ended normally keeps its result. */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "settings.h"
#include "maint.h"

static const char *TAG = "MTEST";
static int fails;
static void check(const char *w, bool ok) { fails += !ok; ESP_LOGI(TAG, "%s %s", ok ? "OK  " : "FAIL", w); }

void app_main(void)
{
    nvs_flash_init();
    static wl_handle_t wl;
    esp_vfs_fat_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 4};
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl("/sdcard", "assets", &mc, &wl));
    settings_init();
    /* the box has the clock from teddyCloud (a run needs it) */
    struct timeval tv = {.tv_sec = 1795400000};
    settimeofday(&tv, NULL);
    const char *r;

    /* 1: the run ends right away (no teddyCloud set up), then hangs while
       switching off: the result stays, with the step after it */
    maint_run();
    maint_last(&r);
    check("1a result of the run", strstr(r, "no teddyCloud") != NULL);
    maint_stage("switching off");
    /* next start without maint_run_end() */
    maint_log_status();
    time_t t = maint_last(&r);
    ESP_LOGI(TAG, "result: '%s'", r);
    check("1b unfinished run reported with its step", t > 0 && strstr(r, "no teddyCloud") && strstr(r, "then: switching off"));

    /* 2: normal end: the result is kept as it is */
    maint_run();
    maint_stage("switching off");
    maint_run_end();
    maint_log_status();
    maint_last(&r);
    ESP_LOGI(TAG, "result: '%s'", r);
    check("2 normal end keeps the result", !strcmp(r, "no teddyCloud set up"));

    /* 3: German texts with text_language = de */
    settings_get_mutable()->text_de = true;
    maint_run();
    maint_run_end();
    maint_last(&r);
    ESP_LOGI(TAG, "result: '%s'", r);
    check("3 German result", !strcmp(r, "kein teddyCloud eingerichtet"));

    ESP_LOGI(TAG, "%s", fails ? "FAILED" : "ALL OK");
    vTaskDelay(portMAX_DELAY);
}
