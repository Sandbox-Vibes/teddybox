#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "memdiag.h"

static const char *TAG = "MEM";

#define MEMDIAG_MAX_TASKS 32

/* runs in the context of the failing allocation, so no malloc and no ESP_LOG here */
static void memdiag_alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    esp_rom_printf("MEM: allocation of %u bytes failed (caps 0x%x) in %s, task '%s': free %u, largest block %u, min free %u\n",
                   (unsigned)size, (unsigned)caps, function_name ? function_name : "?",
                   pcTaskGetName(NULL),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

void memdiag_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("MemPeak", ESP_LOG_INFO);
    heap_caps_register_failed_alloc_callback(memdiag_alloc_failed);
    memdiag_log("boot");
}

static bool memdiag_enabled = true;

void memdiag_enable(bool on)
{
    memdiag_enabled = on;
}

void memdiag_log(const char *where)
{
    if (!memdiag_enabled)
    {
        return;
    }
    ESP_LOGI(TAG, "%s: free %u (min %u) largest %u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void memdiag_log_tasks(void)
{
    if (!memdiag_enabled)
    {
        return;
    }
    static TaskStatus_t tasks[MEMDIAG_MAX_TASKS];
    static char line[512];

    memdiag_log("status");
    UBaseType_t n = uxTaskGetSystemState(tasks, MEMDIAG_MAX_TASKS, NULL);
    int pos = 0;
    for (UBaseType_t i = 0; i < n && pos < (int)sizeof(line) - 40; i++)
    {
        /* ESP-IDF reports the high water mark in bytes */
        pos += snprintf(line + pos, sizeof(line) - pos, "%s%s %u", i ? ", " : "", tasks[i].pcTaskName,
                        (unsigned)tasks[i].usStackHighWaterMark);
    }
    ESP_LOGI(TAG, "stack unused: %s", line);
}
