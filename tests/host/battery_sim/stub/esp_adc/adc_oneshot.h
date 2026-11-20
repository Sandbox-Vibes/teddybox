#pragma once
/* host stub of the IDF 5 ADC oneshot API (only types and what battery.c calls) */
typedef void *adc_oneshot_unit_handle_t;
typedef int adc_channel_t;
typedef int adc_atten_t;
#define ADC_CHANNEL_7 7
#define ADC_CHANNEL_8 8
#define ADC_UNIT_1 1
#define ADC_ATTEN_DB_6 2
#define ADC_ATTEN_DB_12 3
#define ADC_BITWIDTH_12 12
typedef struct { int unit_id; } adc_oneshot_unit_init_cfg_t;
typedef struct { int atten, bitwidth; } adc_oneshot_chan_cfg_t;
static inline int adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, adc_oneshot_unit_handle_t *h) { return 0; }
static inline int adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch, const adc_oneshot_chan_cfg_t *c) { return 0; }
static inline int adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *raw) { *raw = 0; return 0; }
