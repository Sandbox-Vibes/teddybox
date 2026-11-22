/* QEMU test of taskwatch.c: a task that feeds stays alive; a task that stops
   feeding for longer than its timeout makes the box abort; after the
   restart taskwatch_last_reason() names the task. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "taskwatch.h"

static const char *TAG = "TWTEST";
bool wifi_is_connected(void) { return false; }
static int fails;
static void check(const char *w, bool ok) { fails += !ok; ESP_LOGI(TAG, "%s %s", ok ? "OK  " : "FAIL", w); }

static void good(void *arg)
{
    int id = taskwatch_add("good", 2000);
    while (1)
    {
        taskwatch_feed(id);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void stuck(void *arg)
{
    int id = taskwatch_add("stuck", 3000);
    taskwatch_feed(id);
    ESP_LOGI(TAG, "stuck: no more feeding");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    nvs_flash_init();
    nvs_handle_t h;
    uint8_t stage = 0;
    nvs_open("TB_TWTEST", NVS_READWRITE, &h);
    nvs_get_u8(h, "stage", &stage);
    if (stage == 0)
    {
        check("no reason after a normal start", !*taskwatch_last_reason());
        nvs_set_u8(h, "stage", 1);
        nvs_commit(h);
        xTaskCreate(good, "good", 4096, NULL, 5, NULL);
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "good task fed for 5 s, still running");
        xTaskCreate(stuck, "stuck", 4096, NULL, 5, NULL);
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGE(TAG, "FAIL no abort");
        return;
    }
    nvs_set_u8(h, "stage", 0);
    nvs_commit(h);
    ESP_LOGI(TAG, "reset reason %d, taskwatch reason '%s'", esp_reset_reason(), taskwatch_last_reason());
    check("restarted by the watchdog, reason names the task", esp_reset_reason() == ESP_RST_PANIC &&
                                                                  !strncmp(taskwatch_last_reason(), "stuck stuck for", 15));
    ESP_LOGI(TAG, "%s", fails ? "FAILED" : "ALL OK");
}
