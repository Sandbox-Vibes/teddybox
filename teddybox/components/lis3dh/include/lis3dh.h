
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include "esp_err.h"
#include "i2c_bus.h"

typedef struct lis3dh_s *lis3dh_t;

struct lis3dh_s
{
    i2c_bus_handle_t i2c_handle;
    uint16_t range;
    bool valid;
    esp_err_t (*set_data_rate)(lis3dh_t ctx, int rate);
    esp_err_t (*fetch)(lis3dh_t ctx, float *measurements);
};

lis3dh_t lis3dh_init(i2c_bus_handle_t i2c_handle);
esp_err_t lis3dh_get_reg(lis3dh_t ctx, uint8_t reg, uint8_t *val);
esp_err_t lis3dh_set_reg(lis3dh_t ctx, uint8_t reg, uint8_t val);
/* reg | 0x80 for auto increment; in FIFO mode 0xA8 reads samples one after another */
esp_err_t lis3dh_read_burst(lis3dh_t ctx, uint8_t reg, uint8_t *buf, int len);
esp_err_t lis3dh_setup_click(lis3dh_t ctx, uint8_t click_ths);

#ifdef __cplusplus
}
#endif
