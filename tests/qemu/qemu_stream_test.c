/* QEMU test of teddyCloud streams: fake_stream_server.py (port 4444) answers
   like a teddyCloud stream (Content-Length no multiple of 4096) and closes
   after 50000 bytes. A stream must not be resumed with Range, not kept as
   .tmp, and a left-over stream .tmp must not be continued. */
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

static const char *TAG = "STRTEST";
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
static cloud_content_state_t run(uint32_t abort_at, uint32_t *received, bool *stream)
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
    *received = req->received;
    *stream = req->is_stream;
    cloud_content_cleanup(req);
    return s;
}

static size_t varint(uint8_t *p, uint64_t v)
{
    size_t n = 0;
    do
    {
        p[n++] = (v & 0x7f) | (v > 0x7f ? 0x80 : 0);
        v >>= 7;
    } while (v);
    return n;
}

/* TAF file: 4 KB header announcing num_bytes, then extra bytes */
static void write_taf(const char *path, uint64_t num_bytes, size_t extra)
{
    static uint8_t h[4096];
    memset(h, 0, sizeof(h));
    size_t n = 4;
    h[n++] = 0x0a;
    h[n++] = 20;
    n += 20;
    h[n++] = 0x10;
    n += varint(h + n, num_bytes);
    h[n++] = 0x18;
    n += varint(h + n, 0x12345678);
    /* _fill up to 4096 bytes: tag, 2-byte length */
    size_t fill = 4096 - n - 3;
    h[n++] = 0x2a;
    h[n++] = (fill & 0x7f) | 0x80;
    h[n++] = fill >> 7;
    h[0] = h[1] = 0;
    h[2] = 0x0f;
    h[3] = 0xfc;
    FILE *f = fopen(path, "wb");
    fwrite(h, 1, sizeof(h), f);
    memset(h, 0x55, sizeof(h));
    while (extra)
    {
        size_t k = extra < sizeof(h) ? extra : sizeof(h);
        fwrite(h, 1, k, f);
        extra -= k;
    }
    fclose(f);
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
    s->cloud_port = 4444;
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
    unlink(FINAL);
    unlink(TMP);
    uint32_t rx;
    bool stream;

    mkdir("/sdcard/CONTENT", 0777);
    mkdir("/sdcard/CONTENT/A1B2C3D4", 0777);

    /* 0: stream detection from the file header */
    write_taf(TMP, 4096 * 40, 4096);
    check("0a normal TAF is no stream", !pb_file_is_stream(TMP));
    write_taf(TMP, 251658239 - 4096, 8192);
    check("0b stream TAF detected", pb_file_is_stream(TMP));

    /* 1: left-over stream .tmp: not continued (the server answers 416 to a
       Range), the connection ends after 50000 bytes: no automatic resume,
       the .tmp is deleted */
    int64_t t0 = esp_timer_get_time();
    cloud_content_state_t st = run(0, &rx, &stream);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "state %d, received %u, %d ms", st, (unsigned)rx, ms);
    /* full 4 KB blocks reach the card, 50000 bytes were sent */
    check("1a stream recognised, downloaded from the start", stream && rx >= 45056 && rx <= 50000);
    check("1b connection end: no resume", st == CC_STATE_ERROR && ms < 1500);
    check("1c stream .tmp deleted", fsize(TMP) < 0 && fsize(FINAL) < 0);

    /* 2: aborted (tonie removed): deleted as well */
    st = run(20000, &rx, &stream);
    check("2 aborted stream: .tmp deleted", stream && fsize(TMP) < 0 && fsize(FINAL) < 0);

    ESP_LOGI(TAG, "%s", fails ? "FAILED" : "ALL OK");
    while (1)
    {
        vTaskDelay(1000);
    }
}
