#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "mempeak.h"

static const char *TAG = "MemPeak";
static SemaphoreHandle_t sem;
static const char *volatile holder;

bool mempeak_enter(const char *who, uint32_t wait_ms)
{
    if (!sem)
    {
        /* first call happens from one of the app tasks long after boot, races are harmless here */
        sem = xSemaphoreCreateBinary();
        xSemaphoreGive(sem);
    }
    if (xSemaphoreTake(sem, 0) != pdTRUE)
    {
        const char *h = holder;
        ESP_LOGI(TAG, "%s waits for %s", who, h ? h : "?");
        if (xSemaphoreTake(sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
        {
            ESP_LOGW(TAG, "%s: waited %u ms, going on anyway", who, (unsigned)wait_ms);
            return false;
        }
    }
    holder = who;
    return true;
}

void mempeak_leave(bool taken)
{
    if (taken && sem)
    {
        holder = NULL;
        xSemaphoreGive(sem);
    }
}
