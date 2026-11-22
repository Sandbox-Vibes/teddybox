/* QEMU test of the RTNL events to teddyCloud: sends a set of events, the
   host script (mkqrtnl.sh run) checks that teddyCloud reports them on its
   server-sent events (/api/sse). */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "settings.h"
#include "bootsel.h"
#include "cloud.h"
#include "playback.h"
#include "rtnl.h"

static const char *TAG = "RTNLTEST";
extern const uint8_t ca_start[] asm("_binary_tc_ca_der_start");
extern const uint8_t ca_end[] asm("_binary_tc_ca_der_end");
static volatile bool got_ip;
static void on_ip(void *a, esp_event_base_t b, int32_t c, void *d) { got_ip = true; }
bool wifi_is_connected(void) { return got_ip; }

/* low half below 0x80000000: teddyCloud sign-extends it (its bug) */
#define UID 0xE00403501234ABCDULL
#define FINAL "/sdcard/CONTENT/A1B2C3D4/500304E0"
#define TMP FINAL ".tmp"
#define TOTAL 188416

void app_main(void)
{
    nvs_flash_init();
    settings_init();
    bootsel_init();
    static wl_handle_t wl;
    esp_vfs_fat_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 6};
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl("/sdcard", "assets", &mc, &wl));
    FILE *f = fopen("/sdcard/tcca.der", "wb");
    fwrite(ca_start, 1, ca_end - ca_start, f);
    fclose(f);
    settings_t *s = settings_get_mutable();
    strcpy(s->ca_path, "/sdcard/tcca.der");
    strcpy(s->cloud_host, "10.0.2.2");
    s->cloud_port = 443;
    s->cloud_enabled = true;

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
    cloud_init();
    vTaskDelay(pdMS_TO_TICKS(8000)); /* time sync with teddyCloud */
    s->rtnl = true;
    rtnl_init();
    rtnl_tag(UID, true);
    rtnl_audio_id(0x12345678);
    rtnl_event(RTNL3_PLAYBACK_STARTING);
    rtnl_event(RTNL3_PLAYBACK_STARTED);
    rtnl_volume(60);
    rtnl_event(RTNL3_CHARGER_ON);
    rtnl_event(RTNL3_KNOCK_FORWARD);
    vTaskDelay(pdMS_TO_TICKS(3000));
    rtnl_event(RTNL3_PLAYBACK_STOPPED);
    rtnl_tag_removed(UID);
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "EVENTS SENT");
    while (1)
    {
        vTaskDelay(1000);
    }
}
