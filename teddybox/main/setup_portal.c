#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "mbedtls/x509_crt.h"

#include "setup_portal.h"
#include "speaker_eq.h"
#include "settings.h"
#include "wifi.h"
#include "ledman.h"
#include "playback.h"
#include "ota.h"
#include "esp_ota_ops.h"

static const char *TAG = "Setup";

#define AP_IP "192.168.4.1"
#ifndef CA_DIR
#define CA_DIR "/sdcard/teddybox"
#endif
#define CA_FILE CA_DIR "/ca.der"
#define MAX_BODY 4096
#define MAX_SCAN 20

extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_end");

static volatile int64_t last_activity = 0;
static volatile bool saved = false;

bool setup_portal_saved(void)
{
    return saved;
}

static void touch(void)
{
    last_activity = esp_timer_get_time();
}

/* reads the request body into a NUL terminated buffer, caller frees */
static char *read_body(httpd_req_t *req, size_t *len_out)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY)
    {
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf)
    {
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len)
    {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
        {
            continue;
        }
        if (r <= 0)
        {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    *len_out = got;
    return buf;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *s = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!s)
    {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_sendstr(req, s);
    free(s);
    return ret;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *error)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", ok);
    if (error)
    {
        cJSON_AddStringToObject(j, "error", error);
    }
    return send_json(req, j);
}

static esp_err_t page_handler(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* EMBED_TXTFILES appends a NUL byte */
    return httpd_resp_send(req, (const char *)setup_html_start, setup_html_end - setup_html_start - 1);
}

static esp_err_t config_handler(httpd_req_t *req)
{
    touch();
    const settings_t *s = settings_get();
    char ssid[33];
    wifi_get_last_ssid(ssid, sizeof(ssid));
    struct stat st;

    /* never send passwords back, only whether one is set */
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "wifi_ssid", ssid);
    cJSON_AddStringToObject(j, "cloud_host", s->cloud_host);
    cJSON_AddNumberToObject(j, "cloud_port", s->cloud_port);
    cJSON_AddBoolToObject(j, "ca_custom", !strcmp(s->ca_path, CA_FILE) && stat(CA_FILE, &st) == 0);
    cJSON_AddStringToObject(j, "mqtt_host", s->mqtt_host);
    cJSON_AddNumberToObject(j, "mqtt_port", s->mqtt_port);
    cJSON_AddStringToObject(j, "mqtt_user", s->mqtt_user);
    cJSON_AddBoolToObject(j, "mqtt_pass_set", s->mqtt_pass[0] != 0);
    cJSON_AddNumberToObject(j, "lang_id", s->lang_id);
    cJSON_AddNumberToObject(j, "volume_max", s->volume_max);
    cJSON_AddNumberToObject(j, "idle_timeout", s->idle_timeout_s);
    cJSON_AddStringToObject(j, "speaker_eq", speaker_eq_name(s->speaker_eq));
    cJSON_AddNumberToObject(j, "led_brightness", s->led_brightness);
    cJSON_AddNumberToObject(j, "slap", s->slap_level);
    cJSON_AddBoolToObject(j, "slap_reverse", s->slap_reverse);
    cJSON_AddNumberToObject(j, "tilt", s->tilt_level);
    esp_app_desc_t app;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_get_partition_description(running, &app);
    cJSON_AddStringToObject(j, "fw_version", app.version);
    cJSON_AddStringToObject(j, "fw_slot", running->label);
    cJSON_AddBoolToObject(j, "boxine_fallback", s->boxine_fallback);
    cJSON_AddBoolToObject(j, "user_agent_original", s->user_agent_original);
    return send_json(req, j);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    touch();
    cJSON *arr = cJSON_CreateArray();
    wifi_scan_config_t sc = {.show_hidden = false};
    uint16_t n = MAX_SCAN;
    wifi_ap_record_t *recs = calloc(MAX_SCAN, sizeof(wifi_ap_record_t));

    if (recs && esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK)
    {
        /* strongest first, one entry per SSID */
        for (int i = 0; i < n; i++)
        {
            for (int k = i + 1; k < n; k++)
            {
                if (recs[k].rssi > recs[i].rssi)
                {
                    wifi_ap_record_t t = recs[i];
                    recs[i] = recs[k];
                    recs[k] = t;
                }
            }
        }
        for (int i = 0; i < n; i++)
        {
            if (!recs[i].ssid[0])
            {
                continue;
            }
            bool dup = false;
            for (int k = 0; k < i && !dup; k++)
            {
                dup = !strcmp((char *)recs[k].ssid, (char *)recs[i].ssid);
            }
            if (dup)
            {
                continue;
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", (char *)recs[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
            cJSON_AddBoolToObject(o, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(arr, o);
        }
    }
    free(recs);
    return send_json(req, arr);
}

static esp_err_t ca_handler(httpd_req_t *req)
{
    touch();
    size_t len = 0;
    char *body = read_body(req, &len);
    if (!body)
    {
        return send_result(req, false, "Datei fehlt oder ist zu groß");
    }

    /* accepts DER and PEM; PEM needs the terminating NUL in the length */
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    bool pem = strstr(body, "-----BEGIN CERTIFICATE-----") != NULL;
    int r = mbedtls_x509_crt_parse(&crt, (const unsigned char *)body, pem ? len + 1 : len);
    free(body);
    if (r != 0)
    {
        mbedtls_x509_crt_free(&crt);
        return send_result(req, false, "Kein gültiges Zertifikat");
    }

    char subject[128];
    mbedtls_x509_dn_gets(subject, sizeof(subject), &crt.subject);

    mkdir(CA_DIR, S_IRWXU);
    FILE *f = fopen(CA_FILE, "wb");
    bool ok = f && fwrite(crt.raw.p, crt.raw.len, 1, f) == 1;
    if (f)
    {
        ok = (fclose(f) == 0) && ok;
    }
    mbedtls_x509_crt_free(&crt);
    if (!ok)
    {
        return send_result(req, false, "SD-Karte nicht beschreibbar");
    }

    settings_t *s = settings_get_mutable();
    snprintf(s->ca_path, sizeof(s->ca_path), "%s", CA_FILE);
    ESP_LOGI(TAG, "CA saved: %s", subject);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "subject", subject);
    return send_json(req, j);
}

/* copies a JSON string member, dropping control characters so nothing can break the INI file */
static bool get_str(cJSON *o, const char *key, char *dst, size_t size)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsString(v))
    {
        return false;
    }
    size_t n = 0;
    for (const char *p = v->valuestring; *p && n < size - 1; p++)
    {
        if ((unsigned char)*p >= 0x20 && *p != 0x7f)
        {
            dst[n++] = *p;
        }
    }
    dst[n] = 0;
    return true;
}

/* empty or a plain host name / IPv4 address */
static bool valid_host(const char *h)
{
    for (const char *p = h; *p; p++)
    {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_')
        {
            return false;
        }
    }
    return true;
}

static bool get_int(cJSON *o, const char *key, int min, int max, int *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max)
    {
        return false;
    }
    *out = (int)v->valuedouble;
    return true;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    touch();
    size_t len = 0;
    char *body = read_body(req, &len);
    cJSON *o = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!o)
    {
        return send_result(req, false, "Ungültige Anfrage");
    }

    settings_t *s = settings_get_mutable();
    char ssid[33] = "", wpass[65] = "", last[33];
    int v;

    get_str(o, "wifi_ssid", ssid, sizeof(ssid));
    get_str(o, "wifi_pass", wpass, sizeof(wpass));
    wifi_get_last_ssid(last, sizeof(last));

    if (wpass[0] && strlen(wpass) < 8)
    {
        cJSON_Delete(o);
        return send_result(req, false, "Das WLAN-Passwort muss mindestens 8 Zeichen haben");
    }
    if (!ssid[0] && !last[0])
    {
        cJSON_Delete(o);
        return send_result(req, false, "Kein WLAN angegeben");
    }
    /* validate everything before anything is stored */
    char chost[sizeof(s->cloud_host)] = "", mhost[sizeof(s->mqtt_host)] = "";
    get_str(o, "cloud_host", chost, sizeof(chost));
    get_str(o, "mqtt_host", mhost, sizeof(mhost));
    if (!valid_host(chost) || !valid_host(mhost))
    {
        cJSON_Delete(o);
        return send_result(req, false, "Ungültige Adresse, bitte nur IP oder Hostnamen ohne http:// und Pfad");
    }
    /* same network and no new password: keep what is stored */
    if (ssid[0] && (strcmp(ssid, last) || wpass[0]))
    {
        if (wifi_store_credentials(ssid, wpass) != ESP_OK)
        {
            cJSON_Delete(o);
            return send_result(req, false, "WLAN konnte nicht gespeichert werden");
        }
    }

    memcpy(s->cloud_host, chost, sizeof(chost));
    if (get_int(o, "cloud_port", 1, 65535, &v))
    {
        s->cloud_port = v;
    }
    s->cloud_enabled = s->cloud_host[0] != 0;

    memcpy(s->mqtt_host, mhost, sizeof(mhost));
    if (get_int(o, "mqtt_port", 1, 65535, &v))
    {
        s->mqtt_port = v;
    }
    get_str(o, "mqtt_user", s->mqtt_user, sizeof(s->mqtt_user));
    char mpass[sizeof(s->mqtt_pass)];
    if (get_str(o, "mqtt_pass", mpass, sizeof(mpass)) && mpass[0])
    {
        memcpy(s->mqtt_pass, mpass, sizeof(mpass));
    }
    if (!s->mqtt_user[0])
    {
        s->mqtt_pass[0] = 0;
    }

    if (get_int(o, "lang_id", 0, 3, &v))
    {
        s->lang_id = v;
    }
    if (get_int(o, "volume_max", 10, 100, &v))
    {
        s->volume_max = v;
    }
    if (s->volume_default > s->volume_max)
    {
        s->volume_default = s->volume_max;
    }
    if (get_int(o, "idle_timeout", 10, 86400, &v))
    {
        s->idle_timeout_s = v;
    }
    if (get_int(o, "led_brightness", 0, 3, &v))
    {
        s->led_brightness = v;
    }
    if (get_int(o, "slap", 0, 5, &v))
    {
        s->slap_level = v;
    }
    if (get_int(o, "tilt", 0, 5, &v))
    {
        s->tilt_level = v;
    }
    cJSON *sr = cJSON_GetObjectItemCaseSensitive(o, "slap_reverse");
    if (cJSON_IsBool(sr))
    {
        s->slap_reverse = cJSON_IsTrue(sr);
    }
    cJSON *eq = cJSON_GetObjectItemCaseSensitive(o, "speaker_eq");
    if (cJSON_IsString(eq) && speaker_eq_parse(eq->valuestring) >= 0)
    {
        s->speaker_eq = speaker_eq_parse(eq->valuestring);
    }
    cJSON *b = cJSON_GetObjectItemCaseSensitive(o, "boxine_fallback");
    if (cJSON_IsBool(b))
    {
        s->boxine_fallback = cJSON_IsTrue(b);
    }
    b = cJSON_GetObjectItemCaseSensitive(o, "user_agent_original");
    if (cJSON_IsBool(b))
    {
        s->user_agent_original = cJSON_IsTrue(b);
    }
    cJSON_Delete(o);

    if (settings_save_ini(SETTINGS_INI_PATH) != 0)
    {
        return send_result(req, false, "SD-Karte nicht beschreibbar");
    }

    saved = true;
    return send_result(req, true, NULL);
}

/* firmware upload: raw file as request body, "?pd=1" allows using the PD slot */
static esp_err_t firmware_handler(httpd_req_t *req)
{
    touch();
    char query[16] = "", pd[4] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "pd", pd, sizeof(pd));
    bool allow_pd = !strcmp(pd, "1");

    const size_t BUF = 4096;
    uint8_t *buf = malloc(BUF);
    if (!buf)
    {
        return send_result(req, false, "Kein Speicher frei");
    }

    size_t total = req->content_len;
    size_t got = 0;
    const char *err = NULL;
    bool needs_pd = false;
    ota_session_t ota;
    bool begun = false;

    while (got < total && !err)
    {
        /* fill the buffer completely, the first block must contain the whole header */
        size_t fill = 0;
        size_t want = (total - got) < BUF ? (total - got) : BUF;
        while (fill < want)
        {
            int r = httpd_req_recv(req, (char *)buf + fill, want - fill);
            if (r == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue;
            }
            if (r <= 0)
            {
                err = "Verbindung abgebrochen";
                break;
            }
            fill += r;
        }
        if (err)
        {
            break;
        }
        touch();

        if (!begun)
        {
            esp_app_desc_t desc;
            err = ota_check_header(buf, fill, total, &desc);
            if (!err)
            {
                ESP_LOGI(TAG, "Firmware upload: %s %s, %u bytes", desc.project_name, desc.version, (unsigned)total);
                err = ota_session_begin(&ota, total, allow_pd, &needs_pd);
                begun = !err;
            }
            if (err)
            {
                break;
            }
        }
        err = ota_session_write(&ota, buf, fill);
        got += fill;
    }
    free(buf);

    if (!err && begun)
    {
        err = ota_session_finish(&ota);
    }
    else if (begun)
    {
        ota_session_abort(&ota);
    }

    if (err)
    {
        ESP_LOGE(TAG, "Firmware upload failed: %s", err);
        /* drain the rest so the browser gets the answer */
        char tmp[256];
        while (got < total)
        {
            int r = httpd_req_recv(req, tmp, sizeof(tmp));
            if (r <= 0 && r != HTTPD_SOCK_ERR_TIMEOUT)
            {
                break;
            }
            if (r > 0)
            {
                got += r;
            }
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", false);
        cJSON_AddStringToObject(j, "error", err);
        cJSON_AddBoolToObject(j, "needs_pd", needs_pd);
        return send_json(req, j);
    }

    saved = true; /* restarts into the new firmware */
    return send_result(req, true, NULL);
}

/* everything unknown, including the captive portal probes of phones, goes to the setup page */
static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    touch();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t setup_portal_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_handler},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_handler},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler},
        {.uri = "/api/ca", .method = HTTP_POST, .handler = ca_handler},
        {.uri = "/api/save", .method = HTTP_POST, .handler = save_handler},
        {.uri = "/api/firmware", .method = HTTP_POST, .handler = firmware_handler},
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
    {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    touch();
    return httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_handler);
}

/* answers every DNS A query with the box address, so phones detect the captive portal */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        ESP_LOGE(TAG, "DNS socket failed");
        if (sock >= 0)
        {
            close(sock);
        }
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    const uint8_t ip[4] = {192, 168, 4, 1};
    while (true)
    {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &from_len);
        if (len < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1)
        {
            continue; /* not a query with exactly one question */
        }
        int pos = 12;
        while (pos < len && buf[pos])
        {
            pos += buf[pos] + 1;
        }
        if (pos + 5 > len)
        {
            continue;
        }
        uint16_t qtype = (buf[pos + 1] << 8) | buf[pos + 2];
        pos += 5;

        buf[2] = 0x81; /* response, recursion desired */
        buf[3] = 0x80; /* recursion available, no error */
        buf[10] = buf[11] = 0; /* no additional records */
        buf[8] = buf[9] = 0;
        if (qtype == 1) /* A */
        {
            buf[6] = 0;
            buf[7] = 1;
            const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            memcpy(buf + pos, answer, sizeof(answer));
            memcpy(buf + pos + sizeof(answer), ip, 4);
            pos += sizeof(answer) + 4;
        }
        else
        {
            buf[6] = buf[7] = 0; /* no answer for AAAA etc. */
        }
        sendto(sock, buf, pos, 0, (struct sockaddr *)&from, from_len);
    }
}

void setup_portal_start_dns(void)
{
    xTaskCreate(dns_task, "[TB] dns", 3072, NULL, 5, NULL);
}

void setup_portal_run(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = {
        .ap = {
            .channel = 1,
            .max_connection = 3,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    ap.ap.ssid_len = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "TeddyBox-%02X%02X", mac[4], mac[5]);

    /* APSTA so the page can scan for networks */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Hotspot '%s' open, setup page at http://" AP_IP "/", (char *)ap.ap.ssid);

    setup_portal_start_dns();

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 8192;          /* cJSON and certificate parsing */
    hc.max_open_sockets = 5;       /* phones open many connections */
    hc.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &hc) != ESP_OK || setup_portal_register(server) != ESP_OK)
    {
        ESP_LOGE(TAG, "Web server failed to start");
        return;
    }

    /* the setup page works, that is enough to confirm a new firmware;
       a further update would otherwise be refused while on probation */
    ota_confirm_running();

    bool led_set = false;
    while (!saved)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        /* the announcement resets the LED to idle when it ends, show setup mode again */
        if (!led_set && !pb_is_playing())
        {
            ledman_change(SETUP_LED_PATTERN);
            led_set = true;
        }
        if (esp_timer_get_time() - last_activity > (int64_t)SETUP_PORTAL_IDLE_TIMEOUT_MS * 1000)
        {
            ESP_LOGW(TAG, "Nobody used the setup page, giving up");
            httpd_stop(server);
            esp_wifi_stop();
            return;
        }
    }

    ESP_LOGI(TAG, "Settings saved, restarting");
    /* let the response reach the phone */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}
