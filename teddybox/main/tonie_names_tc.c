#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "tonie_names.h"
#include "tonie_names_tc.h"
#include "settings.h"
#include "playback.h"
#include "freshness.h"

static const char *TAG = "Names";

#define TRIED_MAX 64
#define PENDING_MAX 32
#define RESP_MAX 8192
#define STEP_GAP_US (2 * 1000000LL)
#define SCAN_GAP_US (60 * 60 * 1000000LL)
#define ERROR_GAP_US (5 * 60 * 1000000LL)

static uint64_t tried[TRIED_MAX];
static int tried_n;
static uint64_t pending[PENDING_MAX];
static int pending_n;
static int64_t next_step_us, next_scan_us;
static bool login_needed;

static bool was_tried(uint64_t uid)
{
    for (int i = 0; i < tried_n; i++)
    {
        if (tried[i] == uid)
        {
            return true;
        }
    }
    return false;
}

static void mark_tried(uint64_t uid)
{
    if (tried_n < TRIED_MAX)
    {
        tried[tried_n++] = uid;
    }
}

static bool has_tc_name(uint64_t uid)
{
    char n[TONIE_NAME_LEN];
    /* own names count too: no need to ask */
    return tonie_name_lookup(uid, n, sizeof(n));
}

/* tonies on the SD card without a name, not asked yet this session */
static void scan_card(void)
{
    pending_n = 0;
    DIR *top = opendir("/sdcard/CONTENT");
    if (!top)
    {
        return;
    }
    struct dirent *de;
    while ((de = readdir(top)) && pending_n < PENDING_MAX)
    {
        if (strlen(de->d_name) != 8)
        {
            continue;
        }
        char dir_path[32];
        snprintf(dir_path, sizeof(dir_path), "/sdcard/CONTENT/%.8s", de->d_name);
        DIR *sub = opendir(dir_path);
        if (!sub)
        {
            continue;
        }
        struct dirent *fe;
        while ((fe = readdir(sub)) && pending_n < PENDING_MAX)
        {
            uint64_t uid;
            if (strlen(fe->d_name) != 8 || !freshness_uid_from_names(de->d_name, fe->d_name, &uid))
            {
                continue;
            }
            if (!was_tried(uid) && !has_tc_name(uid))
            {
                pending[pending_n++] = uid;
            }
        }
        closedir(sub);
    }
    closedir(top);
}

/* returns false on a connection problem (try again later) */
static bool fetch(uint64_t uid)
{
    const settings_t *s = settings_get();
    char *file = pb_build_filename(uid); /* /sdcard/CONTENT/XXXXXXXX/YYYYYYYY */
    char query[32] = "ruid=";
    int q = 5;
    for (const char *p = file + 16; *p && q < 5 + 16; p++)
    {
        if (*p != '/')
        {
            query[q++] = tolower((unsigned char)*p);
        }
    }
    query[q] = 0;
    free(file);

    esp_http_client_config_t cfg = {
        .host = s->cloud_host,
        .port = s->tc_web_port,
        .path = "/api/getTagInfo",
        .query = query,
        .timeout_ms = 3000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c)
    {
        return false;
    }
    bool conn_ok = false;
    char *buf = NULL;
    if (esp_http_client_open(c, 0) == ESP_OK)
    {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        conn_ok = true;
        if (status == 401 || status == 403)
        {
            ESP_LOGW(TAG, "teddyCloud's web API needs a login (status %d): no names from there", status);
            login_needed = true;
        }
        else if (status == 200 && (buf = malloc(RESP_MAX + 1)))
        {
            int got = 0, r;
            while (got < RESP_MAX && (r = esp_http_client_read(c, buf + got, RESP_MAX - got)) > 0)
            {
                got += r;
            }
            buf[got] = 0;
        }
        else if (status != 200)
        {
            ESP_LOGW(TAG, "getTagInfo: HTTP %d", status);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!conn_ok)
    {
        ESP_LOGW(TAG, "teddyCloud's web interface (%s:%u) not reachable", s->cloud_host, s->tc_web_port);
        return false;
    }
    mark_tried(uid);
    if (!buf)
    {
        return true;
    }

    cJSON *j = cJSON_Parse(buf);
    free(buf);
    cJSON *info = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "tagInfo"), "tonieInfo");
    const char *series = cJSON_GetStringValue(cJSON_GetObjectItem(info, "series"));
    const char *episode = cJSON_GetStringValue(cJSON_GetObjectItem(info, "episode"));
    char name[TONIE_NAME_LEN * 2] = "";
    if (series && *series && episode && *episode)
    {
        snprintf(name, sizeof(name), "%s – %s", series, episode);
    }
    else if (series && *series)
    {
        snprintf(name, sizeof(name), "%s", series);
    }
    else if (episode && *episode)
    {
        snprintf(name, sizeof(name), "%s", episode);
    }
    cJSON_Delete(j);
    if (name[0])
    {
        tonie_names_clean(name, TONIE_NAME_LEN);
        ESP_LOGI(TAG, "%016llX: %s", (unsigned long long)uid, name);
        tonie_names_set(TONIE_TCNAME_FILE, uid, name);
    }
    else
    {
        ESP_LOGI(TAG, "%016llX: teddyCloud knows no name", (unsigned long long)uid);
    }
    return true;
}

void tonie_names_tc_step(void)
{
    const settings_t *s = settings_get();
    int64_t now = esp_timer_get_time();
    if (!s->tc_web_port || !s->cloud_host[0] || login_needed || now < next_step_us)
    {
        return;
    }
    esp_log_level_set(TAG, ESP_LOG_INFO);
    next_step_us = now + STEP_GAP_US;

    /* the tonie that is on the box first (checked once per tonie) */
    static uint64_t checked_uid;
    uint64_t uid = pb_get_current_uid();
    if (uid == checked_uid)
    {
        uid = 0;
    }
    else if (uid)
    {
        checked_uid = uid;
    }
    if (!uid || was_tried(uid) || has_tc_name(uid))
    {
        uid = 0;
        if (!pending_n && now >= next_scan_us)
        {
            next_scan_us = now + SCAN_GAP_US;
            scan_card();
        }
        while (pending_n && !uid)
        {
            uint64_t c = pending[--pending_n];
            if (!was_tried(c))
            {
                uid = c;
            }
        }
    }
    if (uid && !fetch(uid))
    {
        next_step_us = now + ERROR_GAP_US;
    }
}
