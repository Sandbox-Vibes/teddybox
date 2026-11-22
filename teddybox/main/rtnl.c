#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_app_desc.h"
#include "rtnl.h"
#include "cloud.h"
#include "settings.h"
#include "dac3100.h"

static const char *TAG = "RTNL";

/* connection closed after this long without events (frees its RAM) */
#define RTNL_IDLE_CLOSE_MS (60 * 1000)
/* after a failed connection, events are dropped for this long */
#define RTNL_RETRY_MS (30 * 1000)
#define RTNL_QUEUE_LEN 16

typedef struct
{
    uint8_t log3;
    uint8_t len;
    uint32_t a; /* log2: function group, log3: type */
    uint32_t b; /* log2: function */
    int64_t uptime_ms;
    uint32_t datetime;
    uint8_t payload[RTNL_PAYLOAD_MAX];
} rtnl_item_t;

static QueueHandle_t rtnl_queue;
static uint32_t rtnl_sequence;

static void rtnl_put(rtnl_item_t *it)
{
    if (!rtnl_queue || !settings_get()->rtnl)
    {
        return;
    }
    it->uptime_ms = esp_timer_get_time() / 1000;
    it->datetime = (uint32_t)time(NULL);
    xQueueSend(rtnl_queue, it, 0); /* full: dropped */
}

static void rtnl_log2(uint32_t group, uint32_t func, const void *payload, size_t len)
{
    rtnl_item_t it = {.log3 = 0, .a = group, .b = func, .len = len > RTNL_PAYLOAD_MAX ? RTNL_PAYLOAD_MAX : len};
    memcpy(it.payload, payload, it.len);
    rtnl_put(&it);
}

void rtnl_event(uint32_t type)
{
    rtnl_item_t it = {.log3 = 1, .a = type};
    rtnl_put(&it);
}

void rtnl_tag(uint64_t uid, bool valid)
{
    uint8_t p[8];
    rtnl_payload_uid(p, uid);
    rtnl_log2(RTNL2_FUGR_TAG, valid ? RTNL2_FUNC_TAG_VALID : RTNL2_FUNC_TAG_INVALID, p, sizeof(p));
}

void rtnl_tag_removed(uint64_t uid)
{
    uint8_t p[8];
    rtnl_payload_uid(p, uid);
    rtnl_log2(RTNL2_FUGR_TAG, RTNL2_FUNC_TAG_VALID_REMOVED, p, sizeof(p));
}

void rtnl_audio_id(uint32_t audio_id)
{
    uint8_t p[4];
    rtnl_payload_u32le(p, audio_id);
    rtnl_log2(RTNL2_FUGR_AUDIO_A, RTNL2_FUNC_AUDIO_ID, p, sizeof(p));
}

void rtnl_volume(int percent)
{
    uint8_t p[12];
    rtnl_payload_volume(p, dac3100_volume_half_db(percent), dac3100_volume_index(percent));
    rtnl_log2(RTNL2_FUGR_VOLUME, RTNL2_FUNC_VOLUME_CHANGE, p, sizeof(p));
}

static bool rtnl_write(esp_tls_t *tls, const uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len)
    {
        int r = esp_tls_conn_write(tls, buf + done, len - done);
        if (r >= 0)
        {
            done += r;
        }
        else if (r != ESP_TLS_ERR_SSL_WANT_READ && r != ESP_TLS_ERR_SSL_WANT_WRITE)
        {
            return false;
        }
    }
    return true;
}

static size_t rtnl_encode(uint8_t *out, const rtnl_item_t *it)
{
    if (it->log3)
    {
        return rtnl_encode_log3(out, it->datetime, it->a);
    }
    return rtnl_encode_log2(out, it->uptime_ms, rtnl_sequence++, it->a, it->b, it->payload, it->len);
}

static void rtnl_task(void *arg)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_tls_t *tls = NULL;
    int64_t last_us = 0, retry_after_us = 0;
    static uint8_t buf[RTNL_FRAME_MAX];
    rtnl_item_t it;

    while (true)
    {
        if (xQueueReceive(rtnl_queue, &it, tls ? pdMS_TO_TICKS(1000) : portMAX_DELAY) != pdTRUE)
        {
            if (tls && esp_timer_get_time() - last_us > RTNL_IDLE_CLOSE_MS * 1000LL)
            {
                ESP_LOGI(TAG, "Idle, closing the connection");
                esp_tls_conn_destroy(tls);
                tls = NULL;
            }
            continue;
        }
        if (!settings_get()->rtnl)
        {
            continue;
        }
        if (!tls)
        {
            if (esp_timer_get_time() < retry_after_us)
            {
                continue; /* dropped */
            }
            tls = cloud_connect_teddycloud();
            if (!tls)
            {
                /* teddyCloud not (yet) reachable or not the backend in use */
                retry_after_us = esp_timer_get_time() + RTNL_RETRY_MS * 1000LL;
                continue;
            }
            ESP_LOGI(TAG, "Connected to teddyCloud (stack left %u)", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            /* firmware version first, as the original does after connecting */
            const esp_app_desc_t *app = esp_app_get_description();
            char ver[48];
            snprintf(ver, sizeof(ver), "TeddyBox %s", app->version);
            size_t n = rtnl_encode_log2(buf, esp_timer_get_time() / 1000, rtnl_sequence++, RTNL2_FUGR_FIRMWARE,
                                        RTNL2_FUNC_FIRMWARE_VERSION, (const uint8_t *)ver, strlen(ver));
            if (!rtnl_write(tls, buf, n))
            {
                esp_tls_conn_destroy(tls);
                tls = NULL;
                continue;
            }
        }
        size_t n = rtnl_encode(buf, &it);
        if (!rtnl_write(tls, buf, n))
        {
            ESP_LOGW(TAG, "Connection lost");
            esp_tls_conn_destroy(tls);
            tls = NULL;
            continue;
        }
        ESP_LOGD(TAG, "sent %s %u/%u", it.log3 ? "log3" : "log2", (unsigned)it.a, (unsigned)it.b);
        last_us = esp_timer_get_time();
    }
}

void rtnl_init(void)
{
    if (rtnl_queue)
    {
        return;
    }
    rtnl_queue = xQueueCreate(RTNL_QUEUE_LEN, sizeof(rtnl_item_t));
    xTaskCreatePinnedToCore(rtnl_task, "[TB] RTNL", 5000, NULL, 3, NULL, 0);
}
