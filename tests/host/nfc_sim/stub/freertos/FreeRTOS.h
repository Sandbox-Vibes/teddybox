#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1
#define portTICK_RATE_MS 1
#define pdMS_TO_TICKS(x) (x)
#define tskNO_AFFINITY 0
void vTaskDelay(TickType_t ms);
static inline int xTaskCreatePinnedToCore(void *f, const char *n, int s, void *a, int p, void *h, int c) { return 1; }
