/* QEMU test of the "Tonies" tab backend (tonies_portal.c): starts an HTTP
   server on port 80 with the tonie endpoints; mkqtonies.sh run drives it
   with curl through a QEMU port forward. The assets FAT is /sdcard. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_vfs_fat.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "tonies_portal.h"
#include "crashlog.h"
#include "tonie_seen.h"

static const char *TAG = "TNTEST";
static volatile bool got_ip;
static void on_ip(void *a, esp_event_base_t b, int32_t c, void *d) { got_ip = true; }
bool wifi_is_connected(void) { return got_ip; }

void app_main(void)
{
    nvs_flash_init();
    static wl_handle_t wl;
    esp_vfs_fat_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 6};
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl("/sdcard", "assets", &mc, &wl));
    tonie_seen_add(0xE004035011223344ULL);
    tonie_seen_add(0xE00403501234ABCDULL);

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
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 8192;
    hc.max_uri_handlers = 10;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &hc));
    ESP_ERROR_CHECK(tonies_portal_register(server, NULL));
    ESP_ERROR_CHECK(crashlog_register(server, NULL));
    FILE *cf = fopen("/sdcard/tb_crash.txt", "w");
    if (cf)
    {
        fprintf(cf, "TeddyBox test (running now), crash in firmware ELF 12345678 (this one):\n  task cloud, PC 0x42001234\n");
        fclose(cf);
    }
    ESP_LOGI(TAG, "READY");
    while (1)
    {
        vTaskDelay(1000);
    }
}
