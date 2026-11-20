#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

/* setup mode: backup of the certificates and the whole flash, released by
   pressing the small ear (see backup.c) */
esp_err_t backup_register(httpd_handle_t server);
uint32_t backup_flash_size(void);
#ifdef TEDDYBOX_QEMU_TEST
void backup_test_unlock(void);
#endif
