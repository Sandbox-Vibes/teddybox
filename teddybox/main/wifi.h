
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"


void wifi_init(void);
void wifi_save_nvs(void);
void wifi_load_nvs(void);
bool wifi_is_connected(void);
/* stores a network in the original firmware's format (NVS TB_WIFI), tried first on next boot */
esp_err_t wifi_store_credentials(const char *ssid, const char *password);
/* SSID of the most recently stored network, "" if none */
void wifi_get_last_ssid(char *ssid, size_t size);
