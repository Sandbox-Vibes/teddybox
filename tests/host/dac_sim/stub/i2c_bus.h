#pragma once
#include "driver/i2c.h"
typedef void *i2c_bus_handle_t;
i2c_bus_handle_t i2c_bus_create(int port, i2c_config_t *c);
esp_err_t i2c_bus_write_bytes(i2c_bus_handle_t h, int addr, uint8_t *reg, int rl, uint8_t *d, int dl);
esp_err_t i2c_bus_read_bytes(i2c_bus_handle_t h, int addr, uint8_t *reg, int rl, uint8_t *d, int dl);
