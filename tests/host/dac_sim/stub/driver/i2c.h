#pragma once
#include "stubs.h"
typedef int i2c_port_t;
#define I2C_NUM_0 0
#define I2C_MODE_MASTER 0
#define GPIO_PULLUP_ENABLE 1
typedef struct { int mode, sda_io_num, scl_io_num, sda_pullup_en, scl_pullup_en; struct { int clk_speed; } master; } i2c_config_t;
static inline esp_err_t get_i2c_pins(int p, i2c_config_t *c) { return ESP_OK; }
