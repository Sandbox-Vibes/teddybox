/* QEMU test of ofw_patch.c on the original firmware of the base image:
   patch (names, checksum, SHA-256, CA), undo byte for byte, undo after an
   interrupted run, refusals. Works on the copy of the flash image only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "bootsel.h"
#include "cloud.h"
#include "settings.h"
#include "ofw_patch.h"
#include "test_boxine_ca.h"

static const char *TAG = "OFWTEST";
static int fails;
static void check(const char *w, bool ok)
{
    fails += !ok;
    ESP_LOGI(TAG, "%s %s", ok ? "OK  " : "FAIL", w);
}

extern const uint8_t ca_start[] asm("_binary_tc_ca_der_start");
extern const uint8_t ca_end[] asm("_binary_tc_ca_der_end");

#define FLASH_CA "/sdcard/CERT/ca.der" /* the build moves /spiflash/cert/ here */

static void part_digest(const esp_partition_t *p, uint8_t out[32])
{
    static uint8_t buf[4096];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (uint32_t o = 0; o < p->size; o += sizeof(buf))
    {
        esp_partition_read(p, o, buf, sizeof(buf));
        mbedtls_sha256_update(&c, buf, sizeof(buf));
    }
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

static bool file_is(const char *path, const uint8_t *d, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return false;
    }
    uint8_t *b = malloc(len + 1);
    size_t n = fread(b, 1, len + 1, f);
    fclose(f);
    bool same = n == len && !memcmp(b, d, len);
    free(b);
    return same;
}

static void file_put(const char *path, const uint8_t *d, size_t len)
{
    FILE *f = fopen(path, "wb");
    fwrite(d, 1, len, f);
    fclose(f);
}

static bool verifies(const esp_partition_t *p)
{
    esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
    esp_image_metadata_t md;
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) == ESP_OK;
}

void app_main(void)
{
    static wl_handle_t wl;
    esp_vfs_fat_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 6};
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl("/sdcard", "assets", &mc, &wl));
    nvs_flash_init();
    bootsel_init();
    file_put("/sdcard/tcca.der", ca_start, ca_end - ca_start);
    settings_t *s = settings_get_mutable();
    strcpy(s->ca_path, "/sdcard/tcca.der");
    strcpy(s->cloud_host, "10.0.2.2");
    s->cloud_port = 443;

    const esp_partition_t *ofw = bootsel_get_original();
    check("original firmware found", ofw != NULL);
    if (!ofw)
    {
        ESP_LOGE(TAG, "FAILED");
        return;
    }
    /* the base image's CA in flash may be teddyCloud's: put a "Boxine" one there */
    file_put(FLASH_CA, test_boxine_ca_der, test_boxine_ca_der_len);
    unlink(CLOUD_BOXINE_CA_FILE);

    ofw_patch_status_t st;
    ofw_patch_status(&st);
    ESP_LOGI(TAG, "status: valid %d, names %d/%d, patched %d", st.ofw_valid, st.names_found[0], st.names_found[1], st.patched);
    check("image verifies, original names there, not patched", st.ofw_valid && st.names_found[0] > 0 && !st.patched);
    uint8_t d0[32], d1[32];
    part_digest(ofw, d0);

    char err[128];
    check("host too long refused", ofw_patch_apply("toolong.host.example", err, sizeof(err)) != ESP_OK);
    check("bad character refused", ofw_patch_apply("a b", err, sizeof(err)) != ESP_OK);
    s->cloud_port = 8443;
    check("port other than 443 refused", ofw_patch_apply("tc.lan", err, sizeof(err)) != ESP_OK);
    s->cloud_port = 443;
    part_digest(ofw, d1);
    check("refusals left the firmware untouched", !memcmp(d0, d1, 32));

    /* patch */
    int api = st.names_found[0], rtnl = st.names_found[1];
    check("patch", ofw_patch_apply("tc.lan", err, sizeof(err)) == ESP_OK);
    ofw_patch_status(&st);
    check("patched image verifies", verifies(ofw) && st.ofw_valid);
    check("no original names left", st.names_found[0] == 0 && st.names_found[1] == 0);
    check("recorded as patched with the host", st.patched && !strcmp(st.host, "tc.lan"));
    check("teddyCloud CA in flash", file_is(FLASH_CA, ca_start, ca_end - ca_start) && st.flash_ca_teddycloud);
    check("Boxine CA kept on the SD card", file_is(CLOUD_BOXINE_CA_FILE, test_boxine_ca_der, test_boxine_ca_der_len) &&
                                               !strcmp(s->boxine_ca, CLOUD_BOXINE_CA_FILE));
    check("second patch refused", ofw_patch_apply("tc.lan", err, sizeof(err)) != ESP_OK);
    ESP_LOGI(TAG, "replaced %d + %d name(s)", api, rtnl);

    /* undo */
    check("undo", ofw_patch_undo(err, sizeof(err)) == ESP_OK);
    part_digest(ofw, d1);
    check("firmware byte for byte as before", !memcmp(d0, d1, 32));
    check("Boxine CA back in flash", file_is(FLASH_CA, test_boxine_ca_der, test_boxine_ca_der_len));
    ofw_patch_status(&st);
    check("not patched any more", !st.patched && st.names_found[0] == api && st.names_found[1] == rtnl);
    check("second undo refused", ofw_patch_undo(err, sizeof(err)) != ESP_OK);

    /* 13 to 16 characters: only prod.de.tbs.toys, rtnl.bxcl.de stays */
    check("patch with a 14 character address", ofw_patch_apply("192.168.10.133", err, sizeof(err)) == ESP_OK);
    ofw_patch_status(&st);
    check("only the API name replaced", verifies(ofw) && st.names_found[0] == 0 && st.names_found[1] == rtnl);
    check("undo (long address)", ofw_patch_undo(err, sizeof(err)) == ESP_OK);
    part_digest(ofw, d1);
    check("firmware byte for byte as before (long address)", !memcmp(d0, d1, 32));

    /* interrupted run: patched, then one name put back without fixing the image */
    check("patch again", ofw_patch_apply("10.0.2.2", err, sizeof(err)) == ESP_OK);
    {
        /* find one host string and write the original name over it */
        static uint8_t buf[4096];
        bool done = false;
        for (uint32_t o = 0; o + sizeof(buf) <= ofw->size && !done; o += sizeof(buf) - 32)
        {
            esp_partition_read(ofw, o, buf, sizeof(buf));
            for (int i = 0; i + 17 <= (int)sizeof(buf) && !done; i++)
            {
                if (!memcmp(buf + i, "10.0.2.2\0\0\0\0\0\0\0\0\0", 17))
                {
                    uint32_t at = o + i;
                    uint32_t base = at & ~4095u;
                    static uint8_t sec[4096];
                    esp_partition_read(ofw, base, sec, 4096);
                    memcpy(sec + (at - base), "prod.de.tbs.toys", 17);
                    esp_partition_erase_range(ofw, base, 4096);
                    esp_partition_write(ofw, base, sec, 4096);
                    done = true;
                }
            }
        }
        check("simulated interruption: image broken", done && !verifies(ofw));
    }
    check("undo after the interruption", ofw_patch_undo(err, sizeof(err)) == ESP_OK);
    part_digest(ofw, d1);
    check("firmware byte for byte as before (after interruption)", !memcmp(d0, d1, 32));

    /* box already patched by teddyCloud (its CA in flash): CA left, names patched */
    file_put(FLASH_CA, ca_start, ca_end - ca_start);
    unlink(CLOUD_BOXINE_CA_FILE);
    strcpy(s->boxine_ca, "/spiflash/cert/ca.der");
    check("patch with teddyCloud's CA already in flash", ofw_patch_apply("tc.lan", err, sizeof(err)) == ESP_OK);
    struct stat sb;
    check("no Boxine copy made of teddyCloud's CA", stat(CLOUD_BOXINE_CA_FILE, &sb) != 0);
    check("undo without a kept CA: names back, error for the CA",
          ofw_patch_undo(err, sizeof(err)) != ESP_OK && (part_digest(ofw, d1), !memcmp(d0, d1, 32)));
    ofw_patch_status(&st);
    check("record gone after that undo", !st.patched);

    ESP_LOGI(TAG, "%s", fails ? "FAILED" : "ALL OK");
}
