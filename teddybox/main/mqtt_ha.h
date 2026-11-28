#pragma once
#include <stdbool.h>

/* Home Assistant integration over MQTT with discovery, see mqtt_ha_payload.h for topics */

/* starts the client if mqtt_host is configured, call after wifi_init() */
void mqtt_ha_init(void);
/* publishes "offline" and stops the client before power off */
void mqtt_ha_shutdown(void);
/* sends state and diagnostics now and waits (at most timeout_ms) until they
   went out; false if MQTT is not configured or not connected in time */
bool mqtt_ha_flush(int timeout_ms);
