#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

/* setup mode: backup of the certificates and the whole flash, released by
   pressing the small ear (see backup.c) */
esp_err_t backup_register(httpd_handle_t server);
uint32_t backup_flash_size(void);
/* the small ear was pressed for the release (10 minutes); also needed for
   patching the original firmware */
bool backup_unlocked(void);

#ifdef TEDDYBOX_QEMU_TEST
void backup_test_unlock(void);
#endif
