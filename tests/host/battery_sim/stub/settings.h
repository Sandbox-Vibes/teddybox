#pragma once
typedef struct { unsigned bat_critical_mv, bat_low_down_mv, bat_low_up_mv; } settings_t;
const settings_t *settings_get(void);
