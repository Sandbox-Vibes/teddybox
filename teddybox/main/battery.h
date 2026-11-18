#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    BATTERY_UNKNOWN = 0,
    BATTERY_OK,
    BATTERY_LOW,
    BATTERY_CRITICAL,
} battery_state_t;

void battery_init(void);

/* filtered battery voltage in mV, 0 if not measured yet */
uint32_t battery_get_mv(void);
/* true if the charging station (9V input -> 5V buck) is present */
bool battery_is_charging(void);
battery_state_t battery_get_state(void);
const char *battery_state_name(battery_state_t state);
