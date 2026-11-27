#include "readahead.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ReadAhead";

#define RA_TASK_STACK 3072 /* 1.9 KB used */
/* a block that was not there (yet): try again after */
#define RA_RETRY_US 500000

struct readahead
{
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    int n;
    int bs;
    uint32_t limit;
    int32_t *slot_block; /* -1: empty (or being filled) */
    uint8_t **slot_buf;
    uint32_t next; /* first block after the one the reader is at (UINT32_MAX: no block asked for yet) */
    bool quit;
    uint32_t missing_block;
    int64_t missing_until;
    uint32_t hits, misses;
    readahead_read_fn fn;
    void *ctx;
    void (*ctx_free)(void *ctx);
};

/* Block buffers are kept between tonies in a small pool instead of being
   freed: allocated while the heap is still in one piece (first tonie), they
   do not have to be found again later between the pieces a download and
   its TLS handshake leave. An instance that is still ending while the next
   starts (destroy is asynchronous) can use more than the pool: those blocks
   come from the heap and are freed as before. */
#define RA_POOL_MAX 8
static uint8_t *pool[RA_POOL_MAX];
static bool pool_used[RA_POOL_MAX];
static int pool_bs;
static portMUX_TYPE pool_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t *block_get(int bs)
{
    taskENTER_CRITICAL(&pool_mux);
    for (int i = 0; i < RA_POOL_MAX && bs == pool_bs; i++)
    {
        if (pool[i] && !pool_used[i])
        {
            pool_used[i] = true;
            taskEXIT_CRITICAL(&pool_mux);
            return pool[i];
        }
    }
    taskEXIT_CRITICAL(&pool_mux);
    uint8_t *p = heap_caps_malloc(bs, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p)
    {
        return NULL;
    }
    /* a free place in the pool: keep this block from now on */
    taskENTER_CRITICAL(&pool_mux);
    if (!pool_bs)
    {
        pool_bs = bs;
    }
    for (int i = 0; i < RA_POOL_MAX && bs == pool_bs; i++)
    {
        if (!pool[i])
        {
            pool[i] = p;
            pool_used[i] = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&pool_mux);
    return p;
}

static void block_put(uint8_t *p)
{
    if (!p)
    {
        return;
    }
    taskENTER_CRITICAL(&pool_mux);
    for (int i = 0; i < RA_POOL_MAX; i++)
    {
        if (pool[i] == p)
        {
            pool_used[i] = false;
            taskEXIT_CRITICAL(&pool_mux);
            return;
        }
    }
    taskEXIT_CRITICAL(&pool_mux);
    free(p);
}

static void ra_free(readahead_t *ra)
{
    if (ra->slot_buf)
    {
        for (int i = 0; i < ra->n; i++)
        {
            block_put(ra->slot_buf[i]);
        }
    }
    free(ra->slot_buf);
    free(ra->slot_block);
    if (ra->lock)
    {
        vSemaphoreDelete(ra->lock);
    }
    if (ra->ctx_free)
    {
        ra->ctx_free(ra->ctx);
    }
    free(ra);
}

static bool ra_has(readahead_t *ra, uint32_t block)
{
    for (int i = 0; i < ra->n; i++)
    {
        if (ra->slot_block[i] == (int32_t)block)
        {
            return true;
        }
    }
    return false;
}

static void ra_task(void *arg)
{
    readahead_t *ra = arg;

    for (;;)
    {
        int slot = -1;
        uint32_t want = 0;
        TickType_t wait = pdMS_TO_TICKS(1000);

        xSemaphoreTake(ra->lock, portMAX_DELAY);
        if (ra->quit)
        {
            xSemaphoreGive(ra->lock);
            break;
        }
        uint32_t next = ra->next;
        for (uint32_t b = next; next != UINT32_MAX && b < next + ra->n && b < ra->limit; b++)
        {
            if (!ra_has(ra, b))
            {
                want = b;
                slot = 0;
                break;
            }
        }
        if (slot == 0 && want == ra->missing_block && esp_timer_get_time() < ra->missing_until)
        {
            wait = pdMS_TO_TICKS((ra->missing_until - esp_timer_get_time()) / 1000 + 1);
            slot = -1;
        }
        if (slot == 0)
        {
            /* an empty slot or one outside the window [next, next + n):
               there is one, since a block of the window is missing */
            slot = -1;
            for (int i = 0; i < ra->n && slot < 0; i++)
            {
                int32_t b = ra->slot_block[i];
                if (b < 0 || (uint32_t)b < next || (uint32_t)b >= next + ra->n)
                {
                    slot = i;
                }
            }
            if (slot >= 0)
            {
                ra->slot_block[slot] = -1;
            }
        }
        xSemaphoreGive(ra->lock);

        if (slot < 0)
        {
            ulTaskNotifyTake(pdTRUE, wait);
            continue;
        }

        /* without the lock: this may take seconds when the SD card hangs */
        int got = ra->fn(ra->ctx, want, ra->slot_buf[slot]);

        xSemaphoreTake(ra->lock, portMAX_DELAY);
        if (got == ra->bs)
        {
            ra->slot_block[slot] = want;
        }
        else
        {
            ra->missing_block = want;
            ra->missing_until = esp_timer_get_time() + RA_RETRY_US;
        }
        xSemaphoreGive(ra->lock);
    }

    ra_free(ra);
    vTaskDelete(NULL);
}

readahead_t *readahead_create(int blocks, int block_size, uint32_t limit, readahead_read_fn fn, void *ctx,
                              void (*ctx_free)(void *ctx), int prio, int core)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    readahead_t *ra = calloc(1, sizeof(*ra));
    if (!ra)
    {
        if (ctx_free)
        {
            ctx_free(ctx);
        }
        return NULL;
    }
    ra->n = blocks;
    ra->bs = block_size;
    ra->limit = limit;
    ra->fn = fn;
    ra->ctx = ctx;
    ra->ctx_free = ctx_free;
    ra->missing_block = UINT32_MAX;
    ra->next = UINT32_MAX;
    ra->lock = xSemaphoreCreateMutex();
    ra->slot_block = malloc(blocks * sizeof(*ra->slot_block));
    ra->slot_buf = calloc(blocks, sizeof(*ra->slot_buf));
    bool ok = ra->lock && ra->slot_block && ra->slot_buf;
    for (int i = 0; ok && i < blocks; i++)
    {
        ra->slot_block[i] = -1;
        /* single blocks instead of one large buffer: fits into a fragmented heap */
        ra->slot_buf[i] = block_get(block_size);
        ok = ra->slot_buf[i] != NULL;
    }
    if (ok && xTaskCreatePinnedToCore(ra_task, "[TB] readahead", RA_TASK_STACK, ra, prio, &ra->task, core) != pdPASS)
    {
        ok = false;
    }
    if (!ok)
    {
        ESP_LOGW(TAG, "Not enough memory, reading without read-ahead");
        ra_free(ra);
        return NULL;
    }
    return ra;
}

bool readahead_get(readahead_t *ra, uint32_t block, uint8_t *buf)
{
    bool hit = false;

    xSemaphoreTake(ra->lock, portMAX_DELAY);
    ra->next = block + 1;
    for (int i = 0; i < ra->n; i++)
    {
        if (ra->slot_block[i] == (int32_t)block)
        {
            memcpy(buf, ra->slot_buf[i], ra->bs);
            hit = true;
            break;
        }
    }
    if (hit)
    {
        ra->hits++;
    }
    else
    {
        ra->misses++;
    }
    xSemaphoreGive(ra->lock);
    xTaskNotifyGive(ra->task);
    return hit;
}

void readahead_stats(readahead_t *ra, uint32_t *hits, uint32_t *misses)
{
    xSemaphoreTake(ra->lock, portMAX_DELAY);
    *hits = ra->hits;
    *misses = ra->misses;
    xSemaphoreGive(ra->lock);
}

void readahead_destroy(readahead_t *ra)
{
    if (!ra)
    {
        return;
    }
    xSemaphoreTake(ra->lock, portMAX_DELAY);
    ra->quit = true;
    /* still holding the lock: the task cannot have ended yet */
    xTaskNotifyGive(ra->task);
    /* after this the task may free ra at any time */
    xSemaphoreGive(ra->lock);
}
