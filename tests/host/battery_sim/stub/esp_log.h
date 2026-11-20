#pragma once
#include <stdio.h>
#define ESP_LOGI(t, f, ...) printf("I %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOG_INFO 3
static inline void esp_log_level_set(const char *t, int l) {}
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_ERROR_CHECK(x) (void)(x)
#define ESP_LOGW(t, f, ...) printf("W %s: " f "\n", t, ##__VA_ARGS__)
#endif
