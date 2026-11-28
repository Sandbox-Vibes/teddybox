#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "i18n.h"
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
#include "bootsel.h"
#include "tonies_portal.h"
#include "backup.h"
#include "crashlog.h"
#include "speaker_eq.h"
#include "settings.h"
#include "cloud.h"
#include "ofw_patch.h"
#include "wifi.h"
#include "mdns_min.h"
#include "fw_fetch.h"
#include "ledman.h"
#include "playback.h"
#include "ota.h"
#include "esp_ota_ops.h"

static const char *TAG = "Setup";

#define AP_IP "192.168.4.1"

/* reached in the home network (true) or over the own hotspot */
static bool in_home_net = false;
static char lan_ip[16] = "";
static char mdns_host[24] = ""; /* "teddybox-xxxx" */

#ifndef CA_DIR
#define CA_DIR "/sdcard/teddybox"
#endif
#define CA_FILE CA_DIR "/ca.der"
#define BOXINE_CA_FILE CLOUD_BOXINE_CA_FILE
#define MAX_BODY 4096
#define MAX_SCAN 20

extern const uint8_t setup_html_start[] asm("_binary_setup_html_gz_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_gz_end");

/* last use of the page in ms (web server tasks write, the setup loop reads):
   32 bit, so it is written in one store */
static volatile uint32_t last_activity_ms = 0;
static volatile bool saved = false;

static void fetch_finished(void)
{
    saved = true; /* restarts into the new firmware */
}

void setup_portal_touch(void)
{
    last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

static void touch(void)
{
    setup_portal_touch();
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
    /* stored gzip compressed (CMakeLists.txt): every browser takes that */
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)setup_html_start, setup_html_end - setup_html_start);
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
    cJSON_AddNumberToObject(j, "tc_web_port", s->tc_web_port);
    cJSON_AddBoolToObject(j, "ca_custom", !strcmp(s->ca_path, CA_FILE) && stat(CA_FILE, &st) == 0);
    cJSON_AddStringToObject(j, "mqtt_host", s->mqtt_host);
    cJSON_AddNumberToObject(j, "mqtt_port", s->mqtt_port);
    cJSON_AddStringToObject(j, "mqtt_user", s->mqtt_user);
    cJSON_AddBoolToObject(j, "mqtt_pass_set", s->mqtt_pass[0] != 0);
    cJSON_AddNumberToObject(j, "lang_id", s->lang_id);
    cJSON_AddNumberToObject(j, "volume_max", s->volume_max);
    cJSON_AddNumberToObject(j, "volume_max_hdp", s->volume_max_hdp);
    cJSON_AddNumberToObject(j, "volume_min_db", s->volume_min_db);
    cJSON_AddBoolToObject(j, "tc_box_settings", s->tc_box_settings);
    cJSON_AddNumberToObject(j, "idle_timeout", s->idle_timeout_s);
    cJSON_AddStringToObject(j, "speaker_eq", speaker_eq_name(s->speaker_eq));
    cJSON_AddNumberToObject(j, "led_brightness", s->led_brightness);
    cJSON_AddNumberToObject(j, "slap", s->slap_level);
    cJSON_AddBoolToObject(j, "slap_reverse", s->slap_reverse);
    cJSON_AddBoolToObject(j, "ears_poweroff", s->ears_poweroff);
    cJSON_AddStringToObject(j, "text_lang", s->text_de ? "de" : "en");
    cJSON_AddNumberToObject(j, "tilt", s->tilt_level);
    cJSON_AddNumberToObject(j, "update_hour", s->update_hour);
    cJSON_AddNumberToObject(j, "update_battery", s->update_battery);
    cJSON_AddNumberToObject(j, "freshness_interval", s->freshness_min);
    cJSON_AddStringToObject(j, "decoder", s->decoder_celt ? "celt" : "adf");
    cJSON_AddNumberToObject(j, "cpu_mhz", s->cpu_mhz);
    cJSON_AddNumberToObject(j, "download_playing", s->dl_playing_kib_s);
    cJSON_AddBoolToObject(j, "readahead", s->readahead);
    cJSON_AddBoolToObject(j, "rtnl", s->rtnl);
    esp_app_desc_t app;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_get_partition_description(running, &app);
    cJSON_AddStringToObject(j, "fw_version", app.version);
    cJSON_AddStringToObject(j, "fw_slot", running->label);
    cJSON_AddBoolToObject(j, "boxine_fallback", s->boxine_fallback);
    cJSON_AddBoolToObject(j, "boxine_ca_custom", !strcmp(s->boxine_ca, BOXINE_CA_FILE) && stat(BOXINE_CA_FILE, &st) == 0);
    cJSON_AddBoolToObject(j, "user_agent_original", s->user_agent_original);
    /* for the page header and the backup file names */
    uint8_t mac[6];
    char buf[32];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, sizeof(buf), "TeddyBox-%02X%02X", mac[4], mac[5]);
    cJSON_AddStringToObject(j, "ap_ssid", buf);
    cJSON_AddStringToObject(j, "net_mode", in_home_net ? "lan" : "ap");
    cJSON_AddStringToObject(j, "update_repo", TEDDYBOX_REPO);
    if (in_home_net)
    {
        cJSON_AddStringToObject(j, "lan_ip", lan_ip);
        snprintf(buf, sizeof(buf), "%s.local", mdns_host);
        cJSON_AddStringToObject(j, "mdns", buf);
    }
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(j, "box_id", buf);
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

/* POST /api/ca: the CA of teddyCloud; /api/ca?for=boxine: the original CA
   of the Boxine cloud, for the fallback on a box patched for teddyCloud */
static esp_err_t ca_handler(httpd_req_t *req)
{
    touch();
    char query[24] = "", target[8] = "";
    bool boxine = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                  httpd_query_key_value(query, "for", target, sizeof(target)) == ESP_OK && !strcmp(target, "boxine");
    const char *file = boxine ? BOXINE_CA_FILE : CA_FILE;
    size_t len = 0;
    char *body = read_body(req, &len);
    if (!body)
    {
        return send_result(req, false, L("File missing or too big", "Datei fehlt oder ist zu groß"));
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
        return send_result(req, false, L("Not a valid certificate", "Kein gültiges Zertifikat"));
    }

    char subject[128];
    mbedtls_x509_dn_gets(subject, sizeof(subject), &crt.subject);
    if (boxine && cloud_ca_is_teddycloud(crt.raw.p, crt.raw.len))
    {
        mbedtls_x509_crt_free(&crt);
        return send_result(req, false, L("This is the CA of teddyCloud. The Boxine one is certs/client/ca.der in teddyCloud.",
                                         "Das ist die CA von teddyCloud. Die von Boxine ist certs/client/ca.der in teddyCloud."));
    }

    mkdir(CA_DIR, S_IRWXU);
    FILE *f = fopen(file, "wb");
    bool ok = f && fwrite(crt.raw.p, crt.raw.len, 1, f) == 1;
    if (f)
    {
        ok = (fclose(f) == 0) && ok;
    }
    mbedtls_x509_crt_free(&crt);
    if (!ok)
    {
        return send_result(req, false, L("SD card not writable", "SD-Karte nicht beschreibbar"));
    }

    settings_t *s = settings_get_mutable();
    if (boxine)
    {
        snprintf(s->boxine_ca, sizeof(s->boxine_ca), "%s", BOXINE_CA_FILE);
    }
    else
    {
        snprintf(s->ca_path, sizeof(s->ca_path), "%s", CA_FILE);
    }
    ESP_LOGI(TAG, "%s CA saved: %s", boxine ? "Boxine" : "teddyCloud", subject);

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
        return send_result(req, false, L("Invalid request", "Ungültige Anfrage"));
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
        return send_result(req, false, L("The WiFi password needs at least 8 characters", "Das WLAN-Passwort muss mindestens 8 Zeichen haben"));
    }
    if (!ssid[0] && !last[0])
    {
        cJSON_Delete(o);
        return send_result(req, false, L("No WiFi given", "Kein WLAN angegeben"));
    }
    /* validate everything before anything is stored */
    char chost[sizeof(s->cloud_host)] = "", mhost[sizeof(s->mqtt_host)] = "";
    get_str(o, "cloud_host", chost, sizeof(chost));
    get_str(o, "mqtt_host", mhost, sizeof(mhost));
    if (!valid_host(chost) || !valid_host(mhost))
    {
        cJSON_Delete(o);
        return send_result(req, false, L("Invalid address, only an IP or host name without http:// and path", "Ungültige Adresse, bitte nur IP oder Hostnamen ohne http:// und Pfad"));
    }
    /* same network and no new password: keep what is stored */
    if (ssid[0] && (strcmp(ssid, last) || wpass[0]))
    {
        if (wifi_store_credentials(ssid, wpass) != ESP_OK)
        {
            cJSON_Delete(o);
            return send_result(req, false, L("Could not save the WiFi", "WLAN konnte nicht gespeichert werden"));
        }
    }

    memcpy(s->cloud_host, chost, sizeof(chost));
    if (get_int(o, "cloud_port", 1, 65535, &v))
    {
        s->cloud_port = v;
    }
    if (get_int(o, "tc_web_port", 0, 65535, &v))
    {
        s->tc_web_port = v;
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
    if (get_int(o, "volume_max_hdp", 10, 100, &v))
    {
        s->volume_max_hdp = v;
    }
    if (get_int(o, "volume_min_db", -64, -20, &v))
    {
        s->volume_min_db = v;
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
    if (get_int(o, "update_hour", -1, 23, &v))
    {
        s->update_hour = v;
    }
    if (get_int(o, "update_battery", -1, 100, &v) && (v == -1 || v >= 10))
    {
        s->update_battery = v;
    }
    if (get_int(o, "freshness_interval", 15, 720, &v))
    {
        s->freshness_min = v;
    }
    cJSON *sr = cJSON_GetObjectItemCaseSensitive(o, "slap_reverse");
    if (cJSON_IsBool(sr))
    {
        s->slap_reverse = cJSON_IsTrue(sr);
    }
    cJSON *tl = cJSON_GetObjectItemCaseSensitive(o, "text_lang");
    if (cJSON_IsString(tl) && (!strcmp(tl->valuestring, "en") || !strcmp(tl->valuestring, "de")))
    {
        s->text_de = tl->valuestring[0] == 'd';
    }
    cJSON *ep = cJSON_GetObjectItemCaseSensitive(o, "ears_poweroff");
    if (cJSON_IsBool(ep))
    {
        s->ears_poweroff = cJSON_IsTrue(ep);
    }
    cJSON *dec = cJSON_GetObjectItemCaseSensitive(o, "decoder");
    if (cJSON_IsString(dec) && (!strcmp(dec->valuestring, "adf") || !strcmp(dec->valuestring, "celt")))
    {
        s->decoder_celt = !strcmp(dec->valuestring, "celt");
    }
    if (get_int(o, "cpu_mhz", 160, 240, &v) && (v == 160 || v == 240))
    {
        s->cpu_mhz = v;
    }
    if (get_int(o, "download_playing", 0, 4096, &v) && (v == 0 || v >= 32))
    {
        s->dl_playing_kib_s = v;
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
    b = cJSON_GetObjectItemCaseSensitive(o, "tc_box_settings");
    if (cJSON_IsBool(b))
    {
        s->tc_box_settings = cJSON_IsTrue(b);
    }
    b = cJSON_GetObjectItemCaseSensitive(o, "rtnl");
    if (cJSON_IsBool(b))
    {
        s->rtnl = cJSON_IsTrue(b);
    }
    b = cJSON_GetObjectItemCaseSensitive(o, "readahead");
    if (cJSON_IsBool(b))
    {
        s->readahead = cJSON_IsTrue(b);
    }
    cJSON_Delete(o);
    /* fallback just switched on, box not patched: keep its Boxine CA */
    cloud_boxine_ca_keep();

    if (settings_save_ini(SETTINGS_INI_PATH) != 0)
    {
        return send_result(req, false, L("SD card not writable", "SD-Karte nicht beschreibbar"));
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
    httpd_resp_set_hdr(req, "Location", in_home_net ? "/" : "http://" AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

/* start the original firmware (button on the setup page) */
static volatile bool original_requested = false;

static esp_err_t original_handler(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    original_requested = true;
    return ESP_OK;
}

/* teddyCloud patch of the original firmware (ofw_patch.c) */
static esp_err_t ofwpatch_get(httpd_req_t *req)
{
    touch();
    ofw_patch_status_t st;
    ofw_patch_status(&st);
    const settings_t *s = settings_get();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ofw_found", st.ofw_found);
    cJSON_AddBoolToObject(j, "ofw_valid", st.ofw_valid);
    cJSON_AddNumberToObject(j, "names_api", st.names_found[0]);
    cJSON_AddNumberToObject(j, "names_rtnl", st.names_found[1]);
    cJSON_AddBoolToObject(j, "patched", st.patched);
    cJSON_AddStringToObject(j, "host", st.host);
    cJSON_AddBoolToObject(j, "flash_ca_teddycloud", st.flash_ca_teddycloud);
    cJSON_AddBoolToObject(j, "boxine_ca_saved", st.boxine_ca_saved);
    cJSON_AddBoolToObject(j, "port_ok", s->cloud_port == 443);
    /* suggestion: the teddyCloud address, if short enough */
    cJSON_AddStringToObject(j, "suggest", ofw_patch_host_valid(s->cloud_host) ? s->cloud_host : "");
    cJSON_AddBoolToObject(j, "unlocked", backup_unlocked());
    return send_json(req, j);
}

static esp_err_t ofwpatch_post(httpd_req_t *req)
{
    touch();
    if (!backup_unlocked())
    {
        return send_result(req, false, L("Release first: \"Release backup\" and press the small ear",
                                         "Erst freigeben: „Sicherung freigeben“ und das kleine Ohr drücken"));
    }
    size_t len = 0;
    char *body = read_body(req, &len);
    cJSON *o = body ? cJSON_Parse(body) : NULL;
    free(body);
    cJSON *act = o ? cJSON_GetObjectItemCaseSensitive(o, "action") : NULL;
    cJSON *host = o ? cJSON_GetObjectItemCaseSensitive(o, "host") : NULL;
    char err[128] = "";
    esp_err_t r = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(act) && !strcmp(act->valuestring, "apply") && cJSON_IsString(host))
    {
        r = ofw_patch_apply(host->valuestring, err, sizeof(err));
    }
    else if (cJSON_IsString(act) && !strcmp(act->valuestring, "undo"))
    {
        r = ofw_patch_undo(err, sizeof(err));
    }
    else
    {
        snprintf(err, sizeof(err), "Invalid request");
    }
    cJSON_Delete(o);
    /* boxine_ca / ca_path may have changed, also after a partial run */
    settings_save_ini(SETTINGS_INI_PATH);
    return send_result(req, r == ESP_OK, r == ESP_OK ? NULL : err);
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
        {.uri = "/api/original", .method = HTTP_POST, .handler = original_handler},
        {.uri = "/api/ofwpatch", .method = HTTP_GET, .handler = ofwpatch_get},
        {.uri = "/api/ofwpatch", .method = HTTP_POST, .handler = ofwpatch_post},
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
    {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    esp_err_t err = backup_register(server);
    if (err == ESP_OK)
    {
        err = tonies_portal_register(server, touch);
    }
    if (err == ESP_OK)
    {
        err = crashlog_register(server, touch);
    }
    if (err == ESP_OK)
    {
        err = fw_fetch_register(server, touch, fetch_finished);
    }
    if (err != ESP_OK)
    {
        return err;
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

/* mDNS responder for setup mode in the home network (mdns_min.c):
   "teddybox-xxxx.local" -> the box, so the page is reachable by name */
static void mdns_send(int sock, const struct sockaddr_in *to, const uint8_t *buf, int len)
{
    if (len > 0)
    {
        sendto(sock, buf, len, 0, (const struct sockaddr *)to, sizeof(*to));
    }
}

static void mdns_task(void *arg)
{
    uint32_t ip = (uint32_t)(uintptr_t)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(MDNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    struct ip_mreq mreq = {.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP), .imr_interface.s_addr = ip};
    uint8_t ttl = 255;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        ESP_LOGW(TAG, "mDNS not available");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct sockaddr_in group = {.sin_family = AF_INET, .sin_port = htons(MDNS_PORT), .sin_addr.s_addr = inet_addr(MDNS_GROUP)};
    static uint8_t in[512], out[128];
    /* announce twice, as RFC 6762 suggests */
    for (int i = 0; i < 2; i++)
    {
        mdns_send(sock, &group, out, mdns_min_announce(mdns_host, ip, out, sizeof(out)));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "mDNS: %s.local", mdns_host);
    while (1)
    {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&from, &from_len);
        bool legacy = ntohs(from.sin_port) != MDNS_PORT;
        int n = mdns_min_answer(in, len, mdns_host, ip, legacy, out, sizeof(out));
        mdns_send(sock, legacy ? &from : &group, out, n);
    }
}

/* setup mode with stored networks: try the home network first (20 s),
   so the page is reachable from the phone or PC there */
static bool connect_home_net(void)
{
    static wifi_config_t nets[3];
    int n = wifi_stored_networks(nets, 3);
    if (n == 0)
    {
        return false;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mdns_host, sizeof(mdns_host), "teddybox-%02x%02x", mac[4], mac[5]);
    esp_netif_set_hostname(sta, mdns_host);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    for (int i = 0; i < n; i++)
    {
        ESP_LOGI(TAG, "Trying the home network '%.32s'", (char *)nets[i].sta.ssid);
        esp_wifi_set_config(WIFI_IF_STA, &nets[i]);
        esp_wifi_connect();
        for (int t = 0; t < (20000 / n) / 250; t++)
        {
            vTaskDelay(pdMS_TO_TICKS(250));
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr)
            {
                snprintf(lan_ip, sizeof(lan_ip), IPSTR, IP2STR(&ip.ip));
                in_home_net = true;
                ESP_LOGI(TAG, "Setup page in the home network: http://%s/ or http://%s.local/", lan_ip, mdns_host);
                xTaskCreate(mdns_task, "[TB] mdns", 3072, (void *)(uintptr_t)ip.ip.addr, 5, NULL);
                return true;
            }
        }
        esp_wifi_disconnect();
    }
    ESP_LOGW(TAG, "Home network not reachable, opening the hotspot");
    esp_wifi_stop();
    return false;
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

    /* the hotspot only when the home network is not reachable */
    if (!connect_home_net())
    {
        /* APSTA so the page can scan for networks */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "Hotspot '%s' open, setup page at http://" AP_IP "/", (char *)ap.ap.ssid);
        setup_portal_start_dns();
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 8192;          /* cJSON and certificate parsing */
    hc.max_open_sockets = 5;       /* phones open many connections */
    hc.lru_purge_enable = true;
    hc.max_uri_handlers = 30;      /* setup + backup + tonies (+ TAF encoder) + crash log + online update + OFW patch */
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
        if (original_requested)
        {
            ESP_LOGW(TAG, "Starting the original firmware (setup page)");
            /* let the response reach the phone */
            vTaskDelay(pdMS_TO_TICKS(1500));
            bootsel_boot_original();
            /* only returns on error */
            original_requested = false;
        }
        /* the announcement resets the LED to idle when it ends, show setup mode again */
        if (!led_set && !pb_is_playing())
        {
            ledman_change(SETUP_LED_PATTERN);
            led_set = true;
        }
        uint32_t idle_since = last_activity_ms;
        if ((int32_t)((uint32_t)(esp_timer_get_time() / 1000) - idle_since) > (int32_t)SETUP_PORTAL_IDLE_TIMEOUT_MS)
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
