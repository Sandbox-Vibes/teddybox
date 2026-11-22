#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "taskwatch.h"

static const char *TAG = "TASKWATCH";

typedef struct
{
    const char *name;
    uint32_t timeout_ms;
    volatile int64_t last_us;
} taskwatch_entry_t;

static taskwatch_entry_t entries[TASKWATCH_MAX];
static volatile int count;
static esp_timer_handle_t timer;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

/* kept over a software restart (not over power loss) */
#define TASKWATCH_MAGIC 0x54574348u
static RTC_NOINIT_ATTR uint32_t reason_magic;
static RTC_NOINIT_ATTR char reason[48];
static char last_reason[48];
static bool last_reason_read;

const char *taskwatch_last_reason(void)
{
    if (!last_reason_read)
    {
        last_reason_read = true;
        if (reason_magic == TASKWATCH_MAGIC && esp_reset_reason() == ESP_RST_PANIC)
        {
            memcpy(last_reason, reason, sizeof(last_reason) - 1);
        }
        reason_magic = 0;
    }
    return last_reason;
}

static void taskwatch_check(void *arg)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < count; i++)
    {
        taskwatch_entry_t *e = &entries[i];
        int64_t idle_ms = (now - e->last_us) / 1000;
        if (idle_ms > e->timeout_ms)
        {
            snprintf(reason, sizeof(reason), "%s stuck for %lu s", e->name, (unsigned long)(idle_ms / 1000));
            reason_magic = TASKWATCH_MAGIC;
            ESP_LOGE(TAG, "Task %s did not check in for %lu s, restarting", e->name, (unsigned long)(idle_ms / 1000));
            esp_system_abort(reason);
        }
    }
}

int taskwatch_add(const char *name, uint32_t timeout_ms)
{
    /* before anything else: read the reason of the last start before it is overwritten */
    taskwatch_last_reason();
    taskENTER_CRITICAL(&lock);
    bool create = !timer;
    taskEXIT_CRITICAL(&lock);
    if (create)
    {
        const esp_timer_create_args_t args = {.callback = taskwatch_check, .name = "taskwatch"};
        if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_periodic(timer, 1000000) != ESP_OK)
        {
            return -1;
        }
    }
    taskENTER_CRITICAL(&lock);
    int id = count < TASKWATCH_MAX ? count : -1;
    if (id >= 0)
    {
        entries[id].name = name;
        entries[id].timeout_ms = timeout_ms;
        entries[id].last_us = esp_timer_get_time();
        count = id + 1;
    }
    taskEXIT_CRITICAL(&lock);
    if (id < 0)
    {
        return -1;
    }
    ESP_LOGI(TAG, "Watching %s (%lu s)", name, (unsigned long)(timeout_ms / 1000));
    return id;
}

void taskwatch_feed(int id)
{
    if (id >= 0 && id < count)
    {
        entries[id].last_us = esp_timer_get_time();
    }
}
