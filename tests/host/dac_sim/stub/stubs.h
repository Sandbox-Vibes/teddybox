#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_LOGI(t, f, ...) do {} while (0)
#define ESP_LOG_INFO 3
#define esp_log_level_set(t, l) do {} while (0)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
#define portTICK_RATE_MS 1
#define portTICK_PERIOD_MS 1
static inline void vTaskDelay(int x) {}
