#include "taf_verify.h"
#include "rtnl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/sha1.h"
#include "cloud.h"

static const char *TAG = "Verify";

#define VERIFY_STACK 3584
#define VERIFY_PRIO 2
#define VERIFY_BUF 8192
#define TAF_HEADER 4096
/* audio IDs found good since the start */
#define GOOD_MAX 16

typedef struct
{
    char filename[64];
    uint8_t sha1[20];
    uint64_t num_bytes;
    uint32_t audio_id;
} verify_job_t;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool running;

bool taf_verify_running(void)
{
    return running;
}
static uint32_t good[GOOD_MAX];
static int good_n;
static char playing[64];
static char pending_bad[64];

static void rename_bad(const char *filename)
{
    char bad[72];
    snprintf(bad, sizeof(bad), "%s.bad", filename);
    unlink(bad);
    if (rename(filename, bad) == 0)
    {
        ESP_LOGW(TAG, "Moved the damaged file to '%s', it is downloaded again next time", bad);
    }
    else
    {
        ESP_LOGE(TAG, "Could not rename '%s'", filename);
    }
}

static void verify_task(void *arg)
{
    verify_job_t *job = arg;
    /* a download next to the playback needs the RAM (27 Nov: buffers, stack
       and FILE of the check plus RTNL left 1.6 KB, TLS could not take its
       4 KB records); the file is checked the next time it is played */
    /* the RTNL connection closes for the check (see rtnl.c); allocate
       the buffer once it is gone, so the two do not interleave in the heap */
    for (int i = 0; i < 30 && rtnl_is_connected(); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    bool postponed = cloud_download_busy();
    uint8_t *buf = postponed ? NULL : malloc(VERIFY_BUF);
    FILE *f = buf ? fopen(job->filename, "rb") : NULL;
    int result = -1; /* -1 not checked, 0 bad, 1 good */
    int64_t t0 = esp_timer_get_time();

    if (f && fseek(f, TAF_HEADER, SEEK_SET) == 0)
    {
        mbedtls_sha1_context ctx;
        mbedtls_sha1_init(&ctx);
        mbedtls_sha1_starts(&ctx);
        uint64_t left = job->num_bytes;
        bool io_ok = true;
        while (left > 0)
        {
            size_t want = left < VERIFY_BUF ? (size_t)left : VERIFY_BUF;
            size_t got = fread(buf, 1, want, f);
            if (got != want)
            {
                io_ok = false;
                break;
            }
            mbedtls_sha1_update(&ctx, buf, got);
            left -= got;
            if (cloud_download_busy())
            {
                postponed = true;
                io_ok = false;
                break;
            }
            /* background work: give playback and download the SD card */
            vTaskDelay(1);
        }
        uint8_t digest[20];
        mbedtls_sha1_finish(&ctx, digest);
        mbedtls_sha1_free(&ctx);
        if (io_ok)
        {
            result = memcmp(digest, job->sha1, 20) == 0;
        }
        else if (!postponed)
        {
            ESP_LOGW(TAG, "'%s': read error or file too short, not checked", job->filename);
        }
    }
    else if (!postponed)
    {
        ESP_LOGW(TAG, "'%s': cannot open, not checked", job->filename);
    }
    if (f)
    {
        fclose(f);
    }
    free(buf);

    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    bool rename_now = false;
    portENTER_CRITICAL(&lock);
    if (result == 1 && good_n < GOOD_MAX)
    {
        good[good_n++] = job->audio_id;
    }
    if (result == 0)
    {
        if (strcmp(playing, job->filename))
        {
            rename_now = true;
        }
        else
        {
            strlcpy(pending_bad, job->filename, sizeof(pending_bad));
        }
    }
    running = false;
    portEXIT_CRITICAL(&lock);

    if (postponed)
    {
        ESP_LOGI(TAG, "'%s': check postponed, a download needs the RAM", job->filename);
    }
    if (result == 1)
    {
        ESP_LOGI(TAG, "Audio hash OK: '%s' (%d.%d s)", job->filename, ms / 1000, ms % 1000 / 100);
    }
    else if (result == 0)
    {
        ESP_LOGE(TAG, "Audio hash MISMATCH: '%s' is damaged (%d.%d s)", job->filename, ms / 1000, ms % 1000 / 100);
        if (rename_now)
        {
            rename_bad(job->filename);
        }
        else
        {
            ESP_LOGW(TAG, "  renamed to .bad when its playback ends");
        }
    }
    free(job);
    vTaskDelete(NULL);
}

void taf_verify_start(const char *filename, const uint8_t *sha1, size_t sha1_len, uint64_t num_bytes,
                      uint32_t audio_id)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    if (!sha1 || sha1_len != 20 || !num_bytes || strlen(filename) >= sizeof(((verify_job_t *)0)->filename))
    {
        return;
    }
    portENTER_CRITICAL(&lock);
    bool skip = running;
    for (int i = 0; !skip && i < good_n; i++)
    {
        skip = good[i] == audio_id;
    }
    if (!skip)
    {
        running = true;
    }
    portEXIT_CRITICAL(&lock);
    if (skip)
    {
        return;
    }

    verify_job_t *job = calloc(1, sizeof(*job));
    if (!job)
    {
        running = false;
        return;
    }
    strlcpy(job->filename, filename, sizeof(job->filename));
    memcpy(job->sha1, sha1, 20);
    job->num_bytes = num_bytes;
    job->audio_id = audio_id;
    if (xTaskCreatePinnedToCore(verify_task, "[TB] verify", VERIFY_STACK, job, VERIFY_PRIO, NULL, tskNO_AFFINITY) != pdPASS)
    {
        free(job);
        running = false;
        return;
    }
    ESP_LOGI(TAG, "Checking '%s' in the background", filename);
}

void taf_verify_set_playing(const char *filename)
{
    portENTER_CRITICAL(&lock);
    strlcpy(playing, filename ? filename : "", sizeof(playing));
    portEXIT_CRITICAL(&lock);
}

void taf_verify_file_closed(const char *filename)
{
    bool bad = false;
    portENTER_CRITICAL(&lock);
    if (!strcmp(playing, filename))
    {
        playing[0] = 0;
    }
    if (pending_bad[0] && !strcmp(pending_bad, filename))
    {
        bad = true;
        pending_bad[0] = 0;
    }
    portEXIT_CRITICAL(&lock);
    if (bad)
    {
        rename_bad(filename);
    }
}
