#pragma once
/* no NVS on the PC: every open fails, settings keep their defaults */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
static inline esp_err_t nvs_open(const char *n, int m, nvs_handle_t *h) { return -1; }
static inline esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { return -1; }
static inline esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v) { return -1; }
static inline esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *l) { return -1; }
static inline void nvs_close(nvs_handle_t h) {}
