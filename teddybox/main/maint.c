/* Nightly update of the tonies on the SD card, see maint.h */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "board.h"
#include "battery.h"
#include "cloud.h"
#include "freshness.h"
#include "mqtt_ha_payload.h"
#include "playback.h"
#include "settings.h"
#include "wifi.h"
#include "maint.h"

static const char *TAG = "Maint";

#define CONTENT_DIR "/sdcard/CONTENT"
#define SYSTEM_DIR "00000001"
/* same layout as CONTENT (names of at most 8 characters, FATFS_MAX_LFN is 16) */
#define TOKEN_DIR "/sdcard/TOKENS"
#define CLOCK_VALID 1700000000

/* survives deep sleep and software resets; garbage after power-on (magic) */
typedef struct
{
    uint32_t magic;
    time_t next;
    time_t last;
    char result[64];
} maint_rtc_t;
#define MAINT_MAGIC 0x4D41494Eu /* "MAIN" */
static RTC_NOINIT_ATTR maint_rtc_t rtc;

static void rtc_check(void)
{
    if (rtc.magic != MAINT_MAGIC)
    {
        memset(&rtc, 0, sizeof(rtc));
        rtc.magic = MAINT_MAGIC;
    }
    rtc.result[sizeof(rtc.result) - 1] = 0;
}

bool maint_due(void)
{
    rtc_check();
    time_t now = time(NULL);
    /* a minute early is fine, the RTC drifts a little in deep sleep */
    return now > CLOCK_VALID && rtc.next && now >= rtc.next - 60;
}

time_t maint_next_time(time_t now, int hour, int minute, time_t last, int min_gap_s)
{
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    while (t <= now || (last && t - last < min_gap_s))
    {
        tm.tm_mday++;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_isdst = -1;
        t = mktime(&tm);
    }
    return t;
}

void maint_schedule_next(void)
{
    rtc_check();
    int hour = settings_get()->update_hour;
    time_t now = time(NULL);
    if (hour < 0 || hour > 23 || now <= CLOCK_VALID)
    {
        rtc.next = 0;
        return;
    }
    /* keep a planned run; plan again only when it is missing or passed */
    if (rtc.next > now && rtc.next - now < 26 * 3600)
    {
        return;
    }
    /* some minutes later than the full hour, not every box at once */
    rtc.next = maint_next_time(now, hour, esp_random() % 30, rtc.last, 12 * 3600);
    struct tm tm;
    localtime_r(&rtc.next, &tm);
    ESP_LOGI(TAG, "Next update %02d.%02d. %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
}

uint64_t maint_sleep_us(void)
{
    rtc_check();
    time_t now = time(NULL);
    if (!rtc.next || now <= CLOCK_VALID)
    {
        return 0;
    }
    return rtc.next > now ? (uint64_t)(rtc.next - now) * 1000000ULL : 1000000ULL;
}

time_t maint_last(const char **result)
{
    rtc_check();
    *result = rtc.result;
    return rtc.last;
}

void maint_token_names(uint64_t uid, char dir[9], char file[9])
{
    /* byte 0 first, as pb_build_filename() and freshness_uid_from_names() */
    for (int i = 0; i < 4; i++)
    {
        snprintf(dir + 2 * i, 3, "%02X", (unsigned)(uid >> (8 * i)) & 0xFF);
        snprintf(file + 2 * i, 3, "%02X", (unsigned)(uid >> (8 * (i + 4))) & 0xFF);
    }
}

static void token_path(uint64_t uid, char *path, size_t size, bool make_dirs)
{
    char dir[9], file[9];
    maint_token_names(uid, dir, file);
    if (make_dirs)
    {
        mkdir(TOKEN_DIR, 0775);
        snprintf(path, size, TOKEN_DIR "/%s", dir);
        mkdir(path, 0775);
    }
    snprintf(path, size, TOKEN_DIR "/%s/%s", dir, file);
}

bool maint_token_load(uint64_t uid, uint8_t token[32])
{
    char path[64];
    token_path(uid, path, sizeof(path), false);
    FILE *f = fopen(path, "rb");
    bool ok = f && fread(token, 1, 32, f) == 32;
    if (f)
    {
        fclose(f);
    }
    return ok;
}

void maint_token_save(uint64_t uid, const uint8_t token[32])
{
    uint8_t old[32];
    if (maint_token_load(uid, old) && !memcmp(old, token, 32))
    {
        return; /* known, no SD write */
    }
    char path[64];
    token_path(uid, path, sizeof(path), true);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(token, 1, 32, f) != 32)
    {
        ESP_LOGW(TAG, "Could not store the token of %016llX in %s", (unsigned long long)uid, path);
    }
    if (f)
    {
        fclose(f);
    }
}

static bool ears_pressed(void)
{
    return audio_board_ear_big() || audio_board_ear_small();
}

/* a pressed ear means somebody wants to listen: start normally */
static void check_ears(void)
{
    if (ears_pressed())
    {
        ESP_LOGI(TAG, "Ear pressed, stopping the update and starting normally");
        snprintf(rtc.result, sizeof(rtc.result), "abgebrochen (Ohr gedrückt)");
        rtc.last = time(NULL);
        esp_restart();
    }
}

static bool battery_ok(int *percent)
{
    /* the battery task needs a moment for its first values */
    for (int i = 0; i < 50 && battery_get_rest_mv() == 0 && !battery_is_charging(); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (battery_is_charging())
    {
        *percent = -1;
        return true;
    }
    *percent = ha_battery_percent(battery_get_rest_mv(), settings_get()->bat_critical_mv);
    return *percent >= MAINT_MIN_BATTERY_PERCENT;
}

/* downloads one tonie; true if it is complete afterwards */
static bool download(uint64_t uid, const uint8_t token[32], int64_t deadline_us)
{
    cloud_content_req_t *req = cloud_content_download(uid, token);
    if (!req)
    {
        return false;
    }
    uint32_t last_bytes = 0;
    int64_t last_progress = esp_timer_get_time();
    cloud_content_state_t state;
    while ((state = cloud_content_get_state(req)) != CC_STATE_FINISHED && state != CC_STATE_ERROR &&
           state != CC_STATE_ABORTED)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (ears_pressed())
        {
            req->abort = true;
            break;
        }
        int64_t now = esp_timer_get_time();
        if (req->received != last_bytes)
        {
            last_bytes = req->received;
            last_progress = now;
        }
        if (now - last_progress > MAINT_STALL_S * 1000000LL || now > deadline_us ||
            (!battery_is_charging() && battery_get_state() == BATTERY_LOW))
        {
            ESP_LOGW(TAG, "Download of %016llX stopped (no progress, time or battery)", (unsigned long long)uid);
            req->abort = true;
            break;
        }
    }
    while (!req->done)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (req->handle)
    {
        fclose(req->handle);
        req->handle = NULL;
    }
    bool ok = cloud_content_get_state(req) == CC_STATE_FINISHED;
    cloud_content_cleanup(req);
    check_ears();
    return ok;
}

typedef struct
{
    uint64_t uid;
    char path[48];
} candidate_t;
#define MAX_CANDIDATES 32

/* tonies with a new version waiting (<file>.old, set by the freshness check)
   or an unfinished download */
static int find_candidates(candidate_t *c, int max)
{
    DIR *top = opendir(CONTENT_DIR);
    if (!top)
    {
        return 0;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(top)) && n < max)
    {
        if (strlen(de->d_name) != 8 || !strcmp(de->d_name, SYSTEM_DIR))
        {
            continue;
        }
        char dir_name[9];
        snprintf(dir_name, sizeof(dir_name), "%.8s", de->d_name);
        char dir_path[32];
        snprintf(dir_path, sizeof(dir_path), CONTENT_DIR "/%s", dir_name);
        DIR *sub = opendir(dir_path);
        if (!sub)
        {
            continue;
        }
        struct dirent *fe;
        while ((fe = readdir(sub)) && n < max)
        {
            char name[9];
            snprintf(name, sizeof(name), "%.8s", fe->d_name);
            bool old = strlen(fe->d_name) == 12 && !strcasecmp(fe->d_name + 8, ".old");
            if (strlen(fe->d_name) != 8 && !old)
            {
                continue;
            }
            uint64_t uid;
            if (!freshness_uid_from_names(dir_name, name, &uid))
            {
                continue;
            }
            char path[48];
            snprintf(path, sizeof(path), "%s/%s", dir_path, name);
            struct stat st;
            bool exists = stat(path, &st) == 0;
            bool complete = exists && pb_check_file(path) == PB_ERR_GOOD_FILE;
            /* .old: download the new version; plain file: only if unfinished */
            if (complete || (!old && !exists))
            {
                continue;
            }
            bool dup = false;
            for (int i = 0; i < n; i++)
            {
                dup |= c[i].uid == uid;
            }
            if (!dup)
            {
                c[n].uid = uid;
                snprintf(c[n].path, sizeof(c[n].path), "%s", path);
                n++;
            }
        }
        closedir(sub);
    }
    closedir(top);
    return n;
}

static void finish(const char *fmt, int a, int b)
{
    snprintf(rtc.result, sizeof(rtc.result), fmt, a, b);
    rtc.last = time(NULL);
    ESP_LOGI(TAG, "Done: %s", rtc.result);
}

void maint_run(void)
{
    rtc_check();
    esp_log_level_set(TAG, ESP_LOG_INFO);
    const int64_t start = esp_timer_get_time();
    const int64_t deadline = start + MAINT_MAX_RUN_S * 1000000LL;
    rtc.next = 0; /* planned again when going to sleep */

    if (!settings_get()->cloud_enabled)
    {
        finish("kein teddyCloud eingerichtet", 0, 0);
        return;
    }
    int percent;
    if (!battery_ok(&percent))
    {
        ESP_LOGI(TAG, "Battery at %d %%, below %d %%: not today", percent, MAINT_MIN_BATTERY_PERCENT);
        finish("übersprungen (Akku %d %%)", percent, 0);
        return;
    }
    ESP_LOGI(TAG, "Update started (%s)", percent < 0 ? "on the charger" : "on battery");

    /* the cloud task runs the freshness check once it is connected */
    int runs = freshness_runs();
    while (freshness_runs() == runs)
    {
        check_ears();
        if (esp_timer_get_time() - start > 180 * 1000000LL)
        {
            ESP_LOGW(TAG, "No freshness check within 3 minutes (WiFi or teddyCloud not reachable)");
            finish("teddyCloud nicht erreichbar", 0, 0);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    static candidate_t cand[MAX_CANDIDATES];
    int n = find_candidates(cand, MAX_CANDIDATES);
    int done = 0, failed = 0, no_token = 0;
    for (int i = 0; i < n; i++)
    {
        uint8_t token[32];
        if (!maint_token_load(cand[i].uid, token))
        {
            /* placed before tokens were stored: loaded when it is placed again */
            ESP_LOGI(TAG, "  %016llX: no token known yet", (unsigned long long)cand[i].uid);
            no_token++;
            continue;
        }
        if (esp_timer_get_time() > deadline)
        {
            failed += n - i;
            break;
        }
        ESP_LOGI(TAG, "  %016llX: downloading", (unsigned long long)cand[i].uid);
        if (download(cand[i].uid, token, deadline))
        {
            char old[56];
            snprintf(old, sizeof(old), "%s.old", cand[i].path);
            unlink(old);
            done++;
        }
        else
        {
            failed++;
        }
    }

    if (!n)
    {
        finish("alles aktuell", 0, 0);
    }
    else if (!failed)
    {
        finish(no_token ? "%d Tonie(s) aktualisiert, %d ohne Token" : "%d Tonie(s) aktualisiert", done, no_token);
    }
    else
    {
        finish("%d aktualisiert, %d nicht fertig (später weiter)", done, failed);
    }
}
