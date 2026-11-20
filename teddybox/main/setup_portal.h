#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/*
 * Setup mode: the box opens the WiFi hotspot "TeddyBox-XXXX" (no password,
 * only while setup runs) and serves a setup page at http://192.168.4.1/.
 * Phones open it automatically (captive portal). Settings go to
 * /sdcard/teddybox.ini, WiFi credentials to NVS TB_WIFI in the original
 * firmware's format.
 *
 * Started on the first boot (no teddybox.ini yet) or when the big ear is
 * held while switching on.
 */

/* keeps the setup mode alive during long transfers */
void setup_portal_touch(void);

#define SETUP_PORTAL_IDLE_TIMEOUT_MS (15 * 60 * 1000)
#define SETUP_LED_PATTERN "fadeblink blue-green slow"

/* runs until settings were saved (then restarts) or nobody used it for
 * SETUP_PORTAL_IDLE_TIMEOUT_MS (then returns, the caller powers off) */
void setup_portal_run(void);

/* registers the setup page and API on a running server, used by setup_portal_run and tests */
esp_err_t setup_portal_register(httpd_handle_t server);

/* captive portal DNS on UDP port 53 */
void setup_portal_start_dns(void);

/* true once /api/save succeeded */
bool setup_portal_saved(void);
