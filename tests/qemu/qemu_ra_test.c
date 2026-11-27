/* QEMU test of the read-ahead (main/readahead.c) on the assets FAT mounted as
   /sdcard: simulated SD hangs, seek, a file that grows while it is read
   (second read handle next to the download's write handle), ending the
   read-ahead while its task hangs. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "readahead.h"

static const char *TAG = "RATEST";
bool wifi_is_connected(void) { return false; }

#define BS 4096
#define NB 48
static int fails;
static uint8_t blk[BS], want[BS];
static volatile int written;

static void check(const char *what, bool ok)
{
    fails += !ok;
    ESP_LOGI(TAG, "%s %s", ok ? "OK  " : "FAIL", what);
}

static void pattern(uint32_t b, uint8_t *p)
{
    for (int i = 0; i < BS; i++)
    {
        p[i] = (uint8_t)(b * 131 + i * 7 + (i >> 8));
    }
    memcpy(p, &b, 4);
}

typedef struct
{
    const char *name;
    FILE *fd;
    int stall_every; /* simulated SD hang before every n-th block */
    int stall_ms;
} rd_t;

static int rd(void *ctx, uint32_t block, uint8_t *buf)
{
    rd_t *r = ctx;
    if (r->stall_every && block % r->stall_every == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(r->stall_ms));
    }
    int64_t t0 = esp_timer_get_time();
    if (!r->fd)
    {
        r->fd = fopen(r->name, "rb");
    }
    int n = 0;
    if (r->fd && fseek(r->fd, (long)block * BS, SEEK_SET) == 0)
    {
        n = fread(buf, 1, BS, r->fd);
    }
    if (n < BS && r->fd)
    {
        fclose(r->fd);
        r->fd = NULL;
    }
    int ms = (esp_timer_get_time() - t0) / 1000;
    if (ms > 30 && !(r->stall_every && block % r->stall_every == 0))
    {
        ESP_LOGI(TAG, "slow read of block %u: %d bytes, %d ms", (unsigned)block, n, ms);
    }
    return n;
}

static volatile bool rd_freed;
static void rd_free(void *ctx)
{
    rd_t *r = ctx;
    if (r->fd)
    {
        fclose(r->fd);
    }
    rd_freed = true;
}

/* direct read as the playback does it on a miss */
static int direct(const char *name, uint32_t b, uint8_t *buf)
{
    FILE *f = fopen(name, "rb");
    int n = 0;
    if (f && fseek(f, (long)b * BS, SEEK_SET) == 0)
    {
        n = fread(buf, 1, BS, f);
    }
    if (f)
    {
        fclose(f);
    }
    return n;
}

/* consumer: one block every period_ms from..to; counts misses after the first */
static int consume(readahead_t *ra, const char *name, uint32_t from, uint32_t to, int period_ms, int *bad)
{
    int misses = 0;
    for (uint32_t b = from; b < to; b++)
    {
        vTaskDelay(pdMS_TO_TICKS(period_ms));
        bool hit = readahead_get(ra, b, blk);
        if (!hit)
        {
            misses += b != from;
            int64_t t0 = esp_timer_get_time();
            int tries = 0;
            while (direct(name, b, blk) < BS)
            {
                vTaskDelay(pdMS_TO_TICKS(20));
                if (++tries % 50 == 0)
                {
                    ESP_LOGW(TAG, "block %u still not readable (written %d)", (unsigned)b, written);
                }
            }
            ESP_LOGI(TAG, "miss at block %u, direct read %d ms", (unsigned)b, (int)((esp_timer_get_time() - t0) / 1000));
        }
        pattern(b, want);
        if (memcmp(blk, want, BS))
        {
            (*bad)++;
            ESP_LOGE(TAG, "block %u wrong (%s)", (unsigned)b, hit ? "buffer" : "direct");
        }
    }
    return misses;
}

static void writer(void *arg)
{
    FILE *f = fopen("/sdcard/grow.bin", "wb");
    for (int b = 0; b < NB; b++)
    {
        static uint8_t w[BS];
        pattern(b, w);
        fwrite(w, 1, BS, f);
        if (b % 8 == 7)
        {
            fflush(f);
            fsync(fileno(f));
        }
        written = b + 1;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    fclose(f);
    written = NB + 1;
    vTaskDelete(NULL);
}

void app_main(void)
{
    static wl_handle_t wl;
    esp_vfs_fat_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 6};
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl("/sdcard", "assets", &mc, &wl));

    FILE *f = fopen("/sdcard/ra.bin", "wb");
    for (int b = 0; b < NB; b++)
    {
        pattern(b, blk);
        fwrite(blk, 1, BS, f);
    }
    fclose(f);
    size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    /* 1: hangs of 600 ms every 10th block, 8 blocks ahead at 100 ms per block */
    {
        static rd_t r = {.name = "/sdcard/ra.bin", .stall_every = 10, .stall_ms = 600};
        rd_freed = false;
        readahead_t *ra = readahead_create(8, BS, NB, rd, &r, rd_free, 6, tskNO_AFFINITY);
        check("1 created", ra != NULL);
        int bad = 0;
        int m = consume(ra, r.name, 1, 40, 100, &bad);
        ESP_LOGI(TAG, "misses after the start: %d", m);
        check("1 no miss despite 600 ms hangs", m == 0);
        check("1 content", bad == 0);
        /* 2: seek back */
        bad = 0;
        m = consume(ra, r.name, 3, 12, 100, &bad);
        ESP_LOGI(TAG, "misses after the seek: %d", m);
        check("2 seek: at most one more miss", m <= 1);
        check("2 content", bad == 0);
        uint32_t h, mi;
        readahead_stats(ra, &h, &mi);
        ESP_LOGI(TAG, "hits %u misses %u", (unsigned)h, (unsigned)mi);
        readahead_destroy(ra);
        vTaskDelay(pdMS_TO_TICKS(1500));
        check("2 freed", rd_freed);
    }

    /* 3: file grows while it is read (write handle + read handle) */
    unlink("/sdcard/ra.bin"); /* the assets FAT is small */
    {
        static rd_t r = {.name = "/sdcard/grow.bin"};
        rd_freed = false;
        written = 0;
        xTaskCreate(writer, "writer", 4096, NULL, 5, NULL);
        while (written < 4)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        readahead_t *ra = readahead_create(8, BS, NB, rd, &r, rd_free, 6, tskNO_AFFINITY);
        int bad = 0;
        int m = consume(ra, r.name, 1, NB, 60, &bad);
        ESP_LOGI(TAG, "growing file: misses %d", m);
        check("3 content of a growing file", bad == 0);
        check("3 file complete", direct(r.name, NB - 1, blk) == BS);
        while (written <= NB)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        readahead_destroy(ra);
        vTaskDelay(pdMS_TO_TICKS(1500));
        check("3 freed", rd_freed);
    }

    /* 4: end while the task hangs in a read */
    {
        static rd_t r = {.name = "/sdcard/grow.bin", .stall_every = 1, .stall_ms = 2000};
        rd_freed = false;
        readahead_t *ra = readahead_create(8, BS, NB, rd, &r, rd_free, 6, tskNO_AFFINITY);
        readahead_get(ra, 1, blk);
        vTaskDelay(pdMS_TO_TICKS(100));
        int64_t t0 = esp_timer_get_time();
        readahead_destroy(ra);
        int ms = (esp_timer_get_time() - t0) / 1000;
        check("4 destroy does not wait", ms < 50);
        check("4 not freed while hanging", !rd_freed);
        vTaskDelay(pdMS_TO_TICKS(2500));
        check("4 freed after the hang", rd_freed);
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    size_t heap1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "free internal heap before %u after %u", (unsigned)heap0, (unsigned)heap1);
    /* the 8 blocks stay in the pool (kept between tonies), everything else returns */
    check("5 memory returned (except the 8 pool blocks)", heap1 + 1024 + 8 * (BS + 32) >= heap0 && heap1 + 8 * BS <= heap0 + 1024);

    /* 6: the next instances use the pool: no more memory taken for good;
       two at once (destroy is asynchronous) take extra blocks from the heap
       and give them back */
    {
        static rd_t r1 = {.name = "/sdcard/grow.bin"}, r2 = {.name = "/sdcard/grow.bin"};
        readahead_t *a = readahead_create(8, BS, NB, rd, &r1, rd_free, 6, tskNO_AFFINITY);
        size_t with_a = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        readahead_t *b = readahead_create(8, BS, NB, rd, &r2, rd_free, 6, tskNO_AFFINITY);
        size_t with_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        check("6 first instance from the pool", a && with_a + 8 * BS > heap1);
        check("6 second instance from the heap", b && with_b + 8 * BS <= with_a + 1024);
        readahead_destroy(a);
        readahead_destroy(b);
        vTaskDelay(pdMS_TO_TICKS(1500));
        size_t heap2 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "free internal heap after two more: %u", (unsigned)heap2);
        check("6 memory returned again", heap2 + 1024 >= heap1);
    }

    unlink("/sdcard/ra.bin");
    unlink("/sdcard/grow.bin");
    ESP_LOGI(TAG, "RESULT %s (%d failed)", fails ? "FAIL" : "OK", fails);
}
