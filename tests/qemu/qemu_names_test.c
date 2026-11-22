/* QEMU test of tonie_names_tc.c: names of the tonies on the card from
   teddyCloud's web API (mkqnames.sh run puts a custom tonie into the local
   teddyCloud). The assets FAT is /sdcard. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "settings.h"
#include "tonie_names.h"
#include "tonie_names_tc.h"
#include "esp_heap_caps.h"

static const char *TAG = "NMTEST";
static volatile bool got_ip;
static void on_ip(void *a, esp_event_base_t b, int32_t c, void *d) { got_ip = true; }
bool wifi_is_connected(void) { return got_ip; }
static int fails;
static void check(const char *w, bool ok) { fails += !ok; ESP_LOGI(TAG, "%s %s", ok ? "OK  " : "FAIL", w); }

/* stack the name lookup (SD card, two files) needs: run it in a task
   with a painted stack and read the high water mark */
static volatile int lookup_stack = -1;
static void lookup_task(void *arg)
{
    char n[TONIE_NAME_LEN];
    UBaseType_t before = uxTaskGetStackHighWaterMark(NULL);
    tonie_name_lookup(0xE0040350D4C3B2A1ULL, n, sizeof(n));
    tonie_name_lookup(0xE004035000112233ULL, n, sizeof(n));
    lookup_stack = (int)(before - uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

static volatile int step_stack = -1;
static void step_task(void *arg)
{
    UBaseType_t before = uxTaskGetStackHighWaterMark(NULL);
    for (int i = 0; i < 100; i++)
    {
        tonie_names_tc_step();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    step_stack = (int)(before - uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

static void touch_file(const char *dir, const char *file)
{
    char p[64];
    mkdir("/sdcard/CONTENT", 0777);
    snprintf(p, sizeof(p), "/sdcard/CONTENT/%s", dir);
    mkdir(p, 0777);
    snprintf(p, sizeof(p), "/sdcard/CONTENT/%s/%s", dir, file);
    FILE *f = fopen(p, "w");
    fputs("x", f);
    fclose(f);
}

void app_main(void)
{
    nvs_flash_init();
    static wl_handle_t wl;
    esp_vfs_fat_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 6};
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl("/sdcard", "assets", &mc, &wl));
    unlink(TONIE_TCNAME_FILE);
    unlink(TONIE_ALIAS_FILE);
    touch_file("A1B2C3D4", "500304E0"); /* E0040350D4C3B2A1: named in teddyCloud */
    touch_file("33221100", "500304E0"); /* E004035000112233: no name there */

    settings_t *s = settings_get_mutable();
    strcpy(s->cloud_host, "10.0.2.2");
    s->tc_web_port = 80;

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&cfg);
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth));
    esp_netif_attach(netif, esp_eth_new_netif_glue(eth));
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL);
    esp_eth_start(eth);
    while (!got_ip)
    {
        vTaskDelay(100);
    }

    /* 0: little memory (as during download + playback): no request, the
       tonie is not given up but asked again later */
    {
        static void *hog[96];
        int hn = 0;
        size_t big;
        while (hn < 96 && (big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) >= 8 * 1024)
        {
            hog[hn++] = malloc(big - 2048 > 6000 ? big - 2048 : 6000);
        }
        ESP_LOGI(TAG, "memory held in %d blocks, largest free %u", hn,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        for (int i = 0; i < 30; i++)
        {
            tonie_names_tc_step();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        char n0[TONIE_NAME_LEN];
        check("low memory: no request, no name yet", !tonie_name_lookup(0xE0040350D4C3B2A1ULL, n0, sizeof(n0)));
        for (int i = 0; i < hn; i++)
        {
            free(hog[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(31000)); /* BUSY_GAP */
    }
    /* the cloud task calls it every 100 ms; run it in its own task to see its stack use */
    xTaskCreate(step_task, "steps", 8192, NULL, 5, NULL);
    while (step_stack < 0)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "teddyCloud name fetch used %d bytes of stack", step_stack);
    check("name fetch fits the cloud task stack (< 3000 bytes)", step_stack < 3000);
    char n[TONIE_NAME_LEN];
    bool a = tonie_name_lookup(0xE0040350D4C3B2A1ULL, n, sizeof(n));
    ESP_LOGI(TAG, "name: '%s'", n);
    check("name from teddyCloud (series – episode)", a && !strcmp(n, "Testserie – Folge 1"));
    check("no name where teddyCloud has none", !tonie_name_lookup(0xE004035000112233ULL, n, sizeof(n)));
    tonie_names_set(TONIE_ALIAS_FILE, 0xE0040350D4C3B2A1ULL, "Eigener Name");
    check("own name wins", tonie_name_lookup(0xE0040350D4C3B2A1ULL, n, sizeof(n)) && !strcmp(n, "Eigener Name"));
    xTaskCreate(lookup_task, "lookup", 8192, NULL, 5, NULL);
    while (lookup_stack < 0)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "name lookup used %d bytes of stack", lookup_stack);
    check("name lookup fits the mqtt_ha task stack (< 2500 bytes)", lookup_stack < 2500);
    ESP_LOGI(TAG, "%s", fails ? "FAILED" : "ALL OK");
    while (1)
    {
        vTaskDelay(1000);
    }
}
