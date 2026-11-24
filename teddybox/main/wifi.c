/*
 * WiFi station handling.
 *
 * Credentials are shared with the original firmware: NVS namespace "TB_WIFI",
 * blobs SSID0..SSID6 / PW0..PW6 (not zero terminated) and u8 INDEX = slot
 * that was written last + 1. So a box that was set up with the Tonies app
 * connects right away. If nothing works, WPS push button mode is started.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_wps.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "wifi.h"

#define WIFI_NVS_NAMESPACE "TB_WIFI"
#define WIFI_NVS_SLOTS 7
#define MAX_RETRY_ATTEMPTS 2

static const char *TAG = "WiFi";

static esp_wps_config_t wps_config = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);

/* candidates in the order they are tried, most recently stored first */
static wifi_config_t creds[WIFI_NVS_SLOTS];
static int creds_num = 0;
static int creds_idx = 0;
static int retry_num = 0;

/* credentials received by WPS, stored to NVS once the connection works */
static wifi_config_t wps_cred;
static bool wps_pending_save = false;
static bool wps_active = false;

static volatile bool connected = false;

static void wps_start(void)
{
    if (wps_active)
    {
        return;
    }
    ESP_LOGI(TAG, "No working credentials. Press the WPS button on your router.");
    wps_active = true;
    ESP_ERROR_CHECK(esp_wifi_wps_enable(&wps_config));
    ESP_ERROR_CHECK(esp_wifi_wps_start(0));
}

static void wps_restart(void)
{
    esp_wifi_wps_disable();
    wps_active = false;
    wps_start();
}

static bool connect_next(void)
{
    if (creds_idx >= creds_num)
    {
        return false;
    }
    ESP_LOGI(TAG, "Connecting to '%s'", (char *)creds[creds_idx].sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &creds[creds_idx]));
    creds_idx++;
    retry_num = 0;
    esp_wifi_connect();
    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        connected = false;
        if (!connect_next())
        {
            wps_start();
        }
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Associated");
        if (wps_pending_save)
        {
            wifi_save_nvs();
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        connected = false;
        if (wps_active)
        {
            break;
        }
        if (retry_num < MAX_RETRY_ATTEMPTS)
        {
            retry_num++;
            esp_wifi_connect();
        }
        else if (!connect_next())
        {
            /* all known networks failed: start over after WPS attempt */
            creds_idx = 0;
            wps_start();
        }
        break;

    case WIFI_EVENT_STA_WPS_ER_SUCCESS:
    {
        ESP_LOGI(TAG, "WPS succeeded");
        wifi_event_sta_wps_er_success_t *evt = (wifi_event_sta_wps_er_success_t *)event_data;
        memset(&wps_cred, 0, sizeof(wps_cred));
        if (evt && evt->ap_cred_cnt > 0)
        {
            memcpy(wps_cred.sta.ssid, evt->ap_cred[0].ssid, sizeof(evt->ap_cred[0].ssid));
            memcpy(wps_cred.sta.password, evt->ap_cred[0].passphrase, sizeof(evt->ap_cred[0].passphrase));
            esp_wifi_set_config(WIFI_IF_STA, &wps_cred);
        }
        else
        {
            /* single credential: WPS already applied it, read it back */
            esp_wifi_get_config(WIFI_IF_STA, &wps_cred);
        }
        wps_pending_save = true;
        esp_wifi_wps_disable();
        wps_active = false;
        esp_wifi_connect();
        break;
    }

    case WIFI_EVENT_STA_WPS_ER_FAILED:
    case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
        ESP_LOGI(TAG, "WPS failed/timeout, retrying");
        wps_restart();
        break;

    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
    connected = true;
}

bool wifi_is_connected()
{
    return connected;
}

static void slot_key(char *buf, size_t size, const char *prefix, int slot)
{
    snprintf(buf, size, "%s%d", prefix, slot);
}

void wifi_load_nvs(void)
{
    creds_num = 0;
    creds_idx = 0;

    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "No stored WiFi credentials (%s)", esp_err_to_name(err));
        return;
    }

    uint8_t index = 0;
    nvs_get_u8(h, "INDEX", &index);

    /* newest first: INDEX-1, INDEX-2, ... wrapping around all slots */
    for (int n = 0; n < WIFI_NVS_SLOTS; n++)
    {
        int slot = ((int)index - 1 - n + 2 * WIFI_NVS_SLOTS) % WIFI_NVS_SLOTS;
        char key[8];
        wifi_config_t *c = &creds[creds_num];
        memset(c, 0, sizeof(*c));

        size_t len = sizeof(c->sta.ssid);
        slot_key(key, sizeof(key), "SSID", slot);
        if (nvs_get_blob(h, key, c->sta.ssid, &len) != ESP_OK || len == 0)
        {
            continue;
        }
        len = sizeof(c->sta.password) - 1;
        slot_key(key, sizeof(key), "PW", slot);
        if (nvs_get_blob(h, key, c->sta.password, &len) != ESP_OK)
        {
            /* open network */
            c->sta.password[0] = 0;
        }
        ESP_LOGI(TAG, "Slot %d: '%.32s'", slot, (char *)c->sta.ssid);
        creds_num++;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "%d stored network(s)", creds_num);
}

esp_err_t wifi_store_credentials(const char *ssid, const char *password)
{
    size_t ssid_len = strnlen(ssid, 32);
    size_t pw_len = strnlen(password, 64);
    if (ssid_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error (%s) opening NVS", esp_err_to_name(err));
        return err;
    }

    /* reuse the slot if this SSID is already known, else take the next one */
    uint8_t index = 0;
    nvs_get_u8(h, "INDEX", &index);
    int slot = index % WIFI_NVS_SLOTS;
    for (int s = 0; s < WIFI_NVS_SLOTS; s++)
    {
        char key[8];
        uint8_t stored[32];
        size_t len = sizeof(stored);
        slot_key(key, sizeof(key), "SSID", s);
        if (nvs_get_blob(h, key, stored, &len) == ESP_OK && len == ssid_len && !memcmp(stored, ssid, len))
        {
            slot = s;
            break;
        }
    }

    char key[8];
    slot_key(key, sizeof(key), "SSID", slot);
    err = nvs_set_blob(h, key, ssid, ssid_len);
    if (err == ESP_OK)
    {
        slot_key(key, sizeof(key), "PW", slot);
        err = nvs_set_blob(h, key, password, pw_len);
    }
    if (err == ESP_OK)
    {
        /* INDEX = last written slot + 1, so this network is tried first */
        err = nvs_set_u8(h, "INDEX", slot + 1);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to store credentials (%s)", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "Stored '%.32s' in slot %d", ssid, slot);
    }
    return err;
}

void wifi_save_nvs(void)
{
    if (!wps_pending_save)
    {
        return;
    }
    wps_pending_save = false;

    char ssid[33] = {0};
    char pw[65] = {0};
    memcpy(ssid, wps_cred.sta.ssid, 32);
    memcpy(pw, wps_cred.sta.password, 64);
    wifi_store_credentials(ssid, pw);
}

void wifi_get_last_ssid(char *ssid, size_t size)
{
    ssid[0] = 0;
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    {
        return;
    }
    uint8_t index = 0;
    char key[8];
    uint8_t buf[32];
    size_t len = sizeof(buf);
    nvs_get_u8(h, "INDEX", &index);
    slot_key(key, sizeof(key), "SSID", (index + WIFI_NVS_SLOTS - 1) % WIFI_NVS_SLOTS);
    if (nvs_get_blob(h, key, buf, &len) == ESP_OK)
    {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(ssid, buf, n);
        ssid[n] = 0;
    }
    nvs_close(h);
}

int wifi_stored_networks(wifi_config_t *out, int max)
{
    wifi_load_nvs();
    int n = creds_num < max ? creds_num : max;
    memcpy(out, creds, n * sizeof(wifi_config_t));
    return n;
}

void wifi_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    wifi_load_nvs();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    /* name shown in the router instead of "espressif" */
    uint8_t mac[6];
    char hostname[20];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hostname, sizeof(hostname), "teddybox-%02x%02x", mac[4], mac[5]);
    esp_netif_set_hostname(sta_netif, hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    /* credentials live in TB_WIFI, keep the driver from writing its own copy */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}
