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
#include "i18n.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include <sys/time.h>
#include "esp_sleep.h"

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
    /* the run in progress: start time (0 = none) and its current step, so a
       run that never finished is reported at the next start */
    time_t run_start;
    char stage[48];
    /* update_hour the planned run was made for, so a changed setting plans again */
    int plan_hour;
} maint_rtc_t;
#define MAINT_MAGIC 0x4D414950u /* "MAIP": layout with plan_hour */
static RTC_NOINIT_ATTR maint_rtc_t rtc;

static void rtc_check(void)
{
    if (rtc.magic != MAINT_MAGIC)
    {
        memset(&rtc, 0, sizeof(rtc));
        rtc.magic = MAINT_MAGIC;
    }
    rtc.result[sizeof(rtc.result) - 1] = 0;
    rtc.stage[sizeof(rtc.stage) - 1] = 0;
}

static void nvs_store(void);
static int64_t clock_sleep_scale(int64_t real_us);

static void stage_set(const char *fmt, unsigned long long a, int b, int c)
{
    snprintf(rtc.stage, sizeof(rtc.stage), fmt, a, b, c);
    ESP_LOGI(TAG, "Step: %s", rtc.stage);
}

/* a run that started but never reached maint_run_end(): hung, crashed or
   lost power. Reported as its result, with the step it was in. */
static void check_unfinished(void)
{
    if (!rtc.run_start)
    {
        return;
    }
    char prev[64];
    snprintf(prev, sizeof(prev), "%s", rtc.result);
    if (rtc.last >= rtc.run_start && prev[0])
    {
        /* the result was there, it hung or crashed afterwards */
        snprintf(rtc.result, sizeof(rtc.result), L("%.30s, then: %.23s", "%.30s, danach: %.23s"), prev, rtc.stage);
    }
    else
    {
        snprintf(rtc.result, sizeof(rtc.result), L("interrupted at: %.45s", "unterbrochen bei: %.45s"), rtc.stage);
        rtc.last = rtc.run_start;
    }
    ESP_LOGW(TAG, "Last nightly update did not end normally (step: %s)", rtc.stage);
    rtc.run_start = 0;
    nvs_store();
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
    /* keep a planned run; plan again only when it is missing or passed, or
       the hour was changed on the setup page */
    if (rtc.next > now && rtc.next - now < 26 * 3600 && rtc.plan_hour == hour)
    {
        return;
    }
    /* some minutes later than the full hour, not every box at once */
    /* at least an hour after the last run: not twice in one night, but a new
       hour set on the same day still works */
    rtc.next = maint_next_time(now, hour, esp_random() % 30, rtc.last, MAINT_MIN_GAP_S);
    rtc.plan_hour = hour;
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
    if (rtc.next <= now)
    {
        return 1000000ULL;
    }
    /* the sleep timer runs on the same drifting clock */
    return (uint64_t)clock_sleep_scale((int64_t)(rtc.next - now) * 1000000LL);
}

/* the last result also in NVS: the RTC memory is lost on power loss and
   when the firmware is flashed */
#define MAINT_NVS "TB_MAINT"

static void nvs_store(void)
{
    nvs_handle_t h;
    if (nvs_open(MAINT_NVS, NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_i64(h, "last", (int64_t)rtc.last);
        nvs_set_str(h, "result", rtc.result);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_restore(void)
{
    nvs_handle_t h;
    if (rtc.last || nvs_open(MAINT_NVS, NVS_READONLY, &h) != ESP_OK)
    {
        return;
    }
    int64_t last = 0;
    size_t len = sizeof(rtc.result);
    if (nvs_get_i64(h, "last", &last) == ESP_OK && nvs_get_str(h, "result", rtc.result, &len) == ESP_OK)
    {
        rtc.last = (time_t)last;
    }
    else
    {
        rtc.result[0] = 0;
    }
    nvs_close(h);
}

/* ---------------------------------------------------------------------- */
/* deep sleep clock drift                                                  */

int32_t maint_drift_ppm(int64_t box_us, int64_t real_us)
{
    return real_us > 0 ? (int32_t)((box_us - real_us) * 1000000LL / real_us) : 0;
}

int64_t maint_drift_real_to_box(int64_t real_us, int32_t ppm)
{
    return real_us + real_us / 1000000LL * ppm + (real_us % 1000000LL) * ppm / 1000000LL;
}

int64_t maint_drift_box_to_real(int64_t box_us, int32_t ppm)
{
    /* box = real * (1e6 + ppm) / 1e6 */
    int64_t d = 1000000LL + ppm;
    return box_us / d * 1000000LL + (box_us % d) * 1000000LL / d;
}

/* sleeps shorter than this are not measured (time from teddyCloud is in seconds) */
#define DRIFT_MIN_SLEEP_US (2LL * 3600 * 1000000)
/* more than this is no drift but a wrong clock */
#define DRIFT_MAX_PPM 50000

typedef struct
{
    uint32_t magic;
    int32_t ppm;       /* drift used for corrections */
    bool known;        /* ppm measured at least once */
    int64_t ref_us;    /* clock at the last sleep or correction, 0 = none */
    int64_t start_us;  /* clock at sleep, if it was set from teddyCloud: measure from there */
    int32_t start_ppm; /* drift corrected since start_us */
    int32_t last_fix_s; /* last correction at wake (for the log) */
} clock_rtc_t;
#define CLOCK_MAGIC 0x434C4B31u /* "CLK1" */
static RTC_NOINIT_ATTR clock_rtc_t crtc;
/* the clock was set from teddyCloud in this boot */
static bool synced_this_boot;

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static void clock_check(void)
{
    if (crtc.magic != CLOCK_MAGIC)
    {
        memset(&crtc, 0, sizeof(crtc));
        crtc.magic = CLOCK_MAGIC;
    }
}

static int64_t clock_sleep_scale(int64_t real_us)
{
    clock_check();
    return crtc.known ? maint_drift_real_to_box(real_us, crtc.ppm) : real_us;
}

void maint_clock_wake(void)
{
    clock_check();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool from_sleep = cause == ESP_SLEEP_WAKEUP_TIMER || cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1;
    int64_t now = now_us();
    if (!from_sleep || !crtc.ref_us || now <= crtc.ref_us || now / 1000000 <= CLOCK_VALID)
    {
        return;
    }
    if (crtc.known && crtc.ppm)
    {
        int64_t real = crtc.ref_us + maint_drift_box_to_real(now - crtc.ref_us, crtc.ppm);
        struct timeval tv = {.tv_sec = real / 1000000, .tv_usec = real % 1000000};
        settimeofday(&tv, NULL);
        crtc.last_fix_s = (int32_t)((real - now) / 1000000);
        now = real;
    }
    /* the charger check sleeps again right away: correct only from here on */
    crtc.ref_us = now;
}

void maint_clock_sleep(void)
{
    clock_check();
    int64_t now = now_us();
    if (now / 1000000 <= CLOCK_VALID)
    {
        crtc.ref_us = crtc.start_us = 0;
        return;
    }
    crtc.ref_us = now;
    /* measure only from an exact clock; else keep an earlier start (the
       clock was corrected at the wakes since, as far as known) */
    if (synced_this_boot)
    {
        crtc.start_us = now;
        crtc.start_ppm = crtc.known ? crtc.ppm : 0;
    }
}

int32_t maint_clock_drift_ppm(void)
{
    clock_check();
    return crtc.known ? crtc.ppm : 0;
}

static void drift_nvs_store(void)
{
    nvs_handle_t h;
    if (nvs_open(MAINT_NVS, NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_i32(h, "drift_ppm", crtc.ppm);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void drift_nvs_restore(void)
{
    clock_check();
    nvs_handle_t h;
    if (crtc.known || nvs_open(MAINT_NVS, NVS_READONLY, &h) != ESP_OK)
    {
        return;
    }
    int32_t ppm;
    if (nvs_get_i32(h, "drift_ppm", &ppm) == ESP_OK && ppm > -DRIFT_MAX_PPM && ppm < DRIFT_MAX_PPM)
    {
        crtc.ppm = ppm;
        crtc.known = true;
    }
    nvs_close(h);
}

void maint_clock_synced(time_t real)
{
    clock_check();
    int64_t box = now_us();
    int64_t real_us = (int64_t)real * 1000000LL;
    synced_this_boot = true;
    if (!crtc.start_us || real <= CLOCK_VALID)
    {
        return;
    }
    int64_t start = crtc.start_us;
    crtc.start_us = 0; /* once per sleep */
    int64_t passed = real_us - start;
    if (passed < DRIFT_MIN_SLEEP_US)
    {
        return;
    }
    /* what is left after the corrections at the wakes, plus those */
    int32_t measured = crtc.start_ppm + maint_drift_ppm(box - start, passed);
    if (measured <= -DRIFT_MAX_PPM || measured >= DRIFT_MAX_PPM)
    {
        ESP_LOGW(TAG, "Clock: %+lld s after %lld min, not plausible as drift, ignored",
                 (long long)((box - real_us) / 1000000), (long long)(passed / 60000000));
        return;
    }
    int32_t old = crtc.ppm;
    crtc.ppm = crtc.known ? (crtc.ppm + measured) / 2 : measured;
    crtc.known = true;
    ESP_LOGI(TAG, "Clock: %+lld s off after %lld min (corrected for %ld ppm): drift %ld ppm, now using %ld ppm (was %ld)",
             (long long)((box - real_us) / 1000000), (long long)(passed / 60000000), (long)crtc.start_ppm,
             (long)measured, (long)crtc.ppm, (long)old);
    drift_nvs_store();
}

void maint_log_status(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    rtc_check();
    nvs_restore();
    drift_nvs_restore();
    check_unfinished();
    if (crtc.known)
    {
        ESP_LOGI(TAG, "Clock: drift in deep sleep %ld ppm, last correction at wake %+ld s", (long)crtc.ppm, (long)crtc.last_fix_s);
    }
    char last[24] = "none yet", next[24] = "none";
    struct tm tm;
    if (rtc.last > CLOCK_VALID)
    {
        localtime_r(&rtc.last, &tm);
        strftime(last, sizeof(last), "%d.%m. %H:%M", &tm);
    }
    if (rtc.next > CLOCK_VALID)
    {
        localtime_r(&rtc.next, &tm);
        strftime(next, sizeof(next), "%d.%m. %H:%M", &tm);
    }
    ESP_LOGI(TAG, "Nightly update: last %s%s%s, next planned %s", last, rtc.last > CLOCK_VALID ? ": " : "",
             rtc.last > CLOCK_VALID ? rtc.result : "", next);
}

time_t maint_last(const char **result)
{
    rtc_check();
    nvs_restore();
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
        snprintf(rtc.result, sizeof(rtc.result), L("cancelled (ear pressed)", "abgebrochen (Ohr gedrückt)"));
        rtc.last = time(NULL);
        rtc.run_start = 0;
        nvs_store();
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
    int min = settings_get()->update_battery;
    return min >= 0 && *percent >= min;
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
    /* the cloud task ends the request; never wait for it forever */
    for (int i = 0; !req->done; i++)
    {
        if (i == 600)
        {
            ESP_LOGE(TAG, "Download of %016llX does not end after the abort (60 s)", (unsigned long long)uid);
            stage_set(L("download %016llX does not end", "Download %016llX endet nicht"), uid, 0, 0);
        }
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
   or an unfinished download (<file>.tmp, or a partial <file> of older versions) */
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
            /* <file>.old: new version waiting; <file>.tmp: unfinished download */
            bool old = strlen(fe->d_name) == 12 &&
                       (!strcasecmp(fe->d_name + 8, ".old") || !strcasecmp(fe->d_name + 8, ".tmp"));
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
            if (pb_file_is_own(path))
            {
                continue; /* uploaded on the setup page */
            }
            /* left-over stream (power lost while it played): nothing to complete */
            char tmp_path[52];
            snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
            if (pb_file_is_stream(tmp_path))
            {
                ESP_LOGI(TAG, "Deleting left-over stream '%s'", tmp_path);
                unlink(tmp_path);
                continue;
            }
            /* complete but not renamed yet (power lost after the download) */
            pb_finish_tmp(path);
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
    nvs_store();
}

void maint_run(void)
{
    rtc_check();
    esp_log_level_set(TAG, ESP_LOG_INFO);
    const int64_t start = esp_timer_get_time();
    const int64_t deadline = start + MAINT_MAX_RUN_S * 1000000LL;
    rtc.next = 0; /* planned again when going to sleep */
    rtc.run_start = time(NULL);
    stage_set(L("start", "Start"), 0, 0, 0);

    if (!settings_get()->cloud_enabled)
    {
        finish(L("no teddyCloud set up", "kein teddyCloud eingerichtet"), 0, 0);
        return;
    }
    int percent;
    if (!battery_ok(&percent))
    {
        int min = settings_get()->update_battery;
        if (min < 0)
        {
            ESP_LOGI(TAG, "Not on the charger, update_battery = -1: not today");
            finish(L("skipped (not on the charger)", "übersprungen (nicht am Ladegerät)"), 0, 0);
        }
        else
        {
            ESP_LOGI(TAG, "Battery at %d %%, below %d %%: not today", percent, min);
            finish(L("skipped (battery %d %%, min. %d %%)", "übersprungen (Akku %d %%, mind. %d %%)"), percent, min);
        }
        return;
    }
    ESP_LOGI(TAG, "Update started (%s)", percent < 0 ? "on the charger" : "on battery");

    /* the cloud task runs the freshness check once it is connected */
    stage_set(L("waiting for teddyCloud", "warte auf teddyCloud"), 0, 0, 0);
    int runs = freshness_runs();
    while (freshness_runs() == runs)
    {
        check_ears();
        if (esp_timer_get_time() - start > 180 * 1000000LL)
        {
            ESP_LOGW(TAG, "No freshness check within 3 minutes (WiFi or teddyCloud not reachable)");
            finish(L("teddyCloud unreachable", "teddyCloud nicht erreichbar"), 0, 0);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    static candidate_t cand[MAX_CANDIDATES];
    stage_set(L("scanning the SD card", "SD-Karte durchsuchen"), 0, 0, 0);
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
        stage_set(L("download %016llX (%d/%d)", "Download %016llX (%d/%d)"), (unsigned long long)cand[i].uid, i + 1, n);
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
        finish(L("all up to date", "alles aktuell"), 0, 0);
    }
    else if (!failed)
    {
        finish(no_token ? L("%d tonie(s) updated, %d without token", "%d Tonie(s) aktualisiert, %d ohne Token") : L("%d tonie(s) updated", "%d Tonie(s) aktualisiert"), done, no_token);
    }
    else
    {
        finish(L("%d updated, %d unfinished (continued later)", "%d aktualisiert, %d nicht fertig (später weiter)"), done, failed);
    }
}

void maint_stage(const char *what)
{
    snprintf(rtc.stage, sizeof(rtc.stage), "%s", what);
    ESP_LOGI(TAG, "Step: %s", rtc.stage);
}

void maint_run_end(void)
{
    rtc.run_start = 0;
}

/* in update mode nobody watches the main task: if the whole run (including
   sending the result) takes far longer than allowed, note the step it hangs
   in and switch off, silently, as at the end of a normal run */
static void guard_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((MAINT_MAX_RUN_S + MAINT_GUARD_EXTRA_S) * 1000ULL));
    char prev[64];
    snprintf(prev, sizeof(prev), "%s", rtc.result);
    if (rtc.last >= rtc.run_start && prev[0])
    {
        snprintf(rtc.result, sizeof(rtc.result), L("%.30s, hung at: %.21s", "%.30s, hing bei: %.21s"), prev, rtc.stage);
    }
    else
    {
        snprintf(rtc.result, sizeof(rtc.result), L("hung at: %.53s", "hing bei: %.53s"), rtc.stage);
        rtc.last = rtc.run_start ? rtc.run_start : time(NULL);
    }
    ESP_LOGE(TAG, "Nightly update still running after %d min (step: %s): switching off", (MAINT_MAX_RUN_S + MAINT_GUARD_EXTRA_S) / 60, rtc.stage);
    rtc.run_start = 0;
    nvs_store();
    maint_schedule_next();
    audio_board_set_timer_wakeup(maint_sleep_us());
    maint_clock_sleep();
    audio_board_set_charger(battery_is_charging());
    vTaskDelay(pdMS_TO_TICKS(200));
    audio_board_poweroff();
    vTaskDelete(NULL);
}

void maint_guard_start(void)
{
    xTaskCreate(guard_task, "[TB] maint guard", 3072, NULL, 10, NULL);
}
