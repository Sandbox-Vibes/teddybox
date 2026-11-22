/* QEMU test: box settings changed in teddyCloud are taken over (freshness
   check). The first check only stores the baseline; then the host script
   (mkqtcset.sh run) changes slap settings in teddyCloud when the log says
   "CHANGE NOW". A minute lasts 1 s in this build, the freshness interval is 15 s. */
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
#include "freshness.h"
#include "nvs.h"

static const char *TAG = "TCSTEST";
extern const uint8_t ca_start[] asm("_binary_tc_ca_der_start");
extern const uint8_t ca_end[] asm("_binary_tc_ca_der_end");
static volatile bool got_ip;
static void on_ip(void *a, esp_event_base_t b, int32_t c, void *d) { got_ip = true; }
bool wifi_is_connected(void) { return got_ip; }

#define UID 0xE0040350D4C3B2A1ULL
#define FINAL "/sdcard/CONTENT/A1B2C3D4/500304E0"
#define TMP FINAL ".tmp"
#define TOTAL 188416
static int fails;

static long fsize(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void check(const char *what, bool ok)
{
    fails += !ok;
    ESP_LOGI(TAG, "%s %s (final %ld, tmp %ld)", ok ? "OK  " : "FAIL", what, fsize(FINAL), fsize(TMP));
}

/* download; abort_at > 0: abort once that much arrived */
static cloud_content_state_t run(uint32_t abort_at)
{
    static const uint8_t token[32] = {1, 2, 3};
    cloud_content_req_t *req = cloud_content_download(UID, token);
    cloud_content_state_t s;
    while ((s = cloud_content_get_state(req)) != CC_STATE_FINISHED && s != CC_STATE_ERROR && s != CC_STATE_ABORTED)
    {
        if (abort_at && req->received >= abort_at)
        {
            req->abort = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (!req->done)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s = cloud_content_get_state(req);
    cloud_content_cleanup(req);
    return s;
}

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
    nvs_handle_t h;
    if (nvs_open("TB_TCSET", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s->slap_level = 4;
    s->slap_reverse = false;
    s->tc_box_settings = true;
    s->freshness_min = 15;

    /* a tonie on the card, else the check has nothing to ask */
    if (fsize(FINAL) != TOTAL)
    {
        run(0);
    }
    check("0 tonie on the card", fsize(FINAL) == TOTAL);

    int r = freshness_runs();
    while (freshness_runs() <= r)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    size_t len = 0;
    bool stored = nvs_open("TB_TCSET", NVS_READONLY, &h) == ESP_OK && nvs_get_blob(h, "last", NULL, &len) == ESP_OK;
    nvs_close(h);
    check("1 first check: baseline stored, nothing changed", stored && len == sizeof(freshness_settings_t) &&
                                                                 s->slap_level == 4 && !s->slap_reverse);

    ESP_LOGI(TAG, "CHANGE NOW");
    vTaskDelay(pdMS_TO_TICKS(3000));
    r = freshness_runs();
    while (freshness_runs() <= r)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    check("2 slap off and reversed in teddyCloud: taken over", s->slap_level == 0 && s->slap_reverse);

    /* changed on the box: stays while teddyCloud does not change it again */
    s->slap_level = 2;
    r = freshness_runs();
    while (freshness_runs() <= r)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    check("3 box change stays", s->slap_level == 2 && s->slap_reverse);

    ESP_LOGI(TAG, "%s", fails ? "FAILED" : "ALL OK");
    while (1)
    {
        vTaskDelay(1000);
    }
}
