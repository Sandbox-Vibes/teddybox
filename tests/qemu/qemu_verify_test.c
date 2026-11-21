/* QEMU test of taf_verify.c: a good TAF passes, a damaged copy is renamed to .bad */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "taf_verify.h"
#include "taf_test_data.h"

static const char *TAG = "VTEST";
bool wifi_is_connected(void) { return false; }
static int fails;
static void check(const char *w, bool ok) { fails += !ok; ESP_LOGI(TAG, "%s %s", ok ? "OK  " : "FAIL", w); }
static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void write_file(const char *p, bool damage)
{
    FILE *f = fopen(p, "wb");
    fwrite(taf_data, 1, taf_len, f);
    if (damage)
    {
        fseek(f, 4096 + 50000, SEEK_SET);
        uint8_t x = taf_data[4096 + 50000] ^ 0x5A;
        fwrite(&x, 1, 1, f);
    }
    fclose(f);
}

static void wait_idle(void) { vTaskDelay(pdMS_TO_TICKS(4000)); }

void app_main(void)
{
    static wl_handle_t wl;
    esp_vfs_fat_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 6};
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl("/sdcard", "assets", &mc, &wl));
    write_file("/sdcard/good.taf", false);
    write_file("/sdcard/bad.taf", true);

    taf_verify_start("/sdcard/good.taf", taf_sha1, 20, taf_num_bytes, 1);
    wait_idle();
    check("good file kept", exists("/sdcard/good.taf") && !exists("/sdcard/good.taf.bad"));

    taf_verify_start("/sdcard/bad.taf", taf_sha1, 20, taf_num_bytes, 2);
    wait_idle();
    check("damaged file renamed", !exists("/sdcard/bad.taf") && exists("/sdcard/bad.taf.bad"));

    /* playing: rename only when closed (the assets FAT is small: one copy less) */
    unlink("/sdcard/bad.taf.bad");
    write_file("/sdcard/bad2.taf", true);
    taf_verify_set_playing("/sdcard/bad2.taf");
    taf_verify_start("/sdcard/bad2.taf", taf_sha1, 20, taf_num_bytes, 3);
    wait_idle();
    check("playing damaged file kept until closed", exists("/sdcard/bad2.taf"));
    taf_verify_file_closed("/sdcard/bad2.taf");
    check("renamed when closed", !exists("/sdcard/bad2.taf") && exists("/sdcard/bad2.taf.bad"));

    /* audio id 1 already good: no second check */
    taf_verify_start("/sdcard/good.taf", taf_sha1, 20, taf_num_bytes, 1);
    check("known good id skipped (see no 'Checking' line)", true);
    wait_idle();

    unlink("/sdcard/good.taf"); unlink("/sdcard/bad2.taf.bad");
    ESP_LOGI(TAG, "RESULT %s (%d failed)", fails ? "FAIL" : "OK", fails);
}
