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

    /* 4: a changed update_hour plans again (it used to keep the old plan) */
    settings_get_mutable()->update_hour = 3;
    maint_schedule_next();
    uint64_t a = maint_sleep_us();
    settings_get_mutable()->update_hour = (localtime(&(time_t){time(NULL)})->tm_hour + 2) % 24;
    maint_schedule_next();
    uint64_t b = maint_sleep_us();
    ESP_LOGI(TAG, "planned in %llu min, after the change in %llu min", a / 60000000ULL, b / 60000000ULL);
    check("4 changed hour planned again", a != b && b < 3ULL * 3600 * 1000000ULL);

    /* 3: German texts with text_language = de */
    settings_get_mutable()->text_de = true;
    maint_run();
    maint_run_end();
    maint_last(&r);
    ESP_LOGI(TAG, "result: '%s'", r);
    check("3 German result", !strcmp(r, "kein teddyCloud eingerichtet"));

    /* 5: clock drift: synced, sleep 14.5 h, the clock gained 9 min. The
       time from teddyCloud measures it, NVS keeps it, the sleep timer is
       scaled with it. (No real deep sleep in QEMU: the clock is moved.) */
    settings_get_mutable()->update_hour = 3;
    maint_clock_synced(time(NULL));
    maint_clock_sleep();
    time_t real = time(NULL) + 14 * 3600 + 30 * 60;
    tv.tv_sec = real + 9 * 60;
    settimeofday(&tv, NULL);
    maint_clock_synced(real); /* logs the measurement */
    tv.tv_sec = real;
    settimeofday(&tv, NULL);
    maint_schedule_next();
    uint64_t scaled = maint_sleep_us();
    maint_log_status(); /* drift from NVS/RTC in the log */
    check("5a drift measured: 9 min in 14.5 h = 10344 ppm", maint_clock_drift_ppm() == 10344);
    /* the timer for the time to the next run, about 1.0344 times longer */
    uint64_t want_real = scaled * 1000000ULL / 1010344ULL;
    ESP_LOGI(TAG, "sleep timer %llu s for %llu s", scaled / 1000000ULL, want_real / 1000000ULL);
    check("5b sleep timer lengthened by the drift", scaled > want_real && scaled - want_real > want_real / 100);
    /* second sleep of 3 h, corrected at the wake: right on time, the
       residual is 0: measured again 10344, average unchanged */
    maint_clock_synced(time(NULL));
    maint_clock_sleep();
    real = time(NULL) + 3 * 3600;
    tv.tv_sec = real;
    settimeofday(&tv, NULL);
    maint_clock_synced(real);
    check("5c corrected clock keeps the drift", maint_clock_drift_ppm() == 10344);
    /* third sleep, 1 h with 1 min off: too short, not measured */
    maint_clock_sleep();
    tv.tv_sec = real + 3600 + 60;
    settimeofday(&tv, NULL);
    maint_clock_synced(real + 3600);
    check("5d sleep under 2 h not measured", maint_clock_drift_ppm() == 10344);
    /* fourth: a jump of 2 h in 3 h is no drift */
    tv.tv_sec = real + 3600;
    settimeofday(&tv, NULL);
    maint_clock_synced(real + 3600);
    maint_clock_sleep();
    tv.tv_sec = real + 3600 + 5 * 3600;
    settimeofday(&tv, NULL);
    maint_clock_synced(real + 3600 + 3 * 3600);
    check("5e implausible jump ignored", maint_clock_drift_ppm() == 10344);

    ESP_LOGI(TAG, "%s", fails ? "FAILED" : "ALL OK");
    vTaskDelay(portMAX_DELAY);
}
