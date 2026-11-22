#pragma once
/*
 * "Tonies" tab of the setup page: list of the tonie files on the SD card,
 * upload of a finished TAF file for a tonie (checked: header, length,
 * SHA-1), delete, and protection against replacement by teddyCloud.
 * An uploaded file gets a "<file>.own" marker: the freshness check does not
 * report it and the nightly update does not touch it.
 */
#include "esp_http_server.h"

/* touch: keeps the setup mode alive during long uploads */
esp_err_t tonies_portal_register(httpd_handle_t server, void (*touch)(void));
