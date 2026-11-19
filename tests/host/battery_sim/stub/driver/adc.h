#pragma once
typedef int adc1_channel_t;
#define ADC1_CHANNEL_8 8
#define ADC1_CHANNEL_7 7
#define ADC_WIDTH_BIT_12 0
#define ADC_ATTEN_DB_6 2
#define ADC_ATTEN_DB_11 3
#define ADC_UNIT_1 1
static inline void adc1_config_width(int w) {}
static inline void adc1_config_channel_atten(int c, int a) {}
static inline int esp_adc_cal_characterize(int u, int a, int w, int v, void *c) { return 0; }
static inline void xTaskCreate(void *f, const char *n, int s, void *a, int p, void *h) {}
static inline void vTaskDelay(int t) {}
