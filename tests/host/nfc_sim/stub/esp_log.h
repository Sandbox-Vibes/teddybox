#pragma once
#include <stdio.h>
extern int sim_verbose; extern double sim_now(void);
#define esp_log_level_set(a,b)
#define ESP_LOGI(t,f,...) do{ if(sim_verbose) printf("%8.3fs I %s " f "\n", sim_now(), t, ##__VA_ARGS__);}while(0)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE(t,f,...) do{}while(0)
#define ESP_LOGD(t,f,...) do{}while(0)
