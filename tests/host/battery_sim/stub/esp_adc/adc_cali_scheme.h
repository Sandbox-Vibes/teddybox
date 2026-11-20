#pragma once
typedef struct { int unit_id, chan, atten, bitwidth; } adc_cali_curve_fitting_config_t;
static inline int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c, adc_cali_handle_t *h) { *h = 0; return -1; }
