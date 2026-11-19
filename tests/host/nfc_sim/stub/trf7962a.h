#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
typedef struct trf7962a_s *trf7962a_t;
esp_err_t trf7962a_reset(trf7962a_t ctx);
esp_err_t trf7962a_xmit(trf7962a_t ctx, uint8_t *tx, uint8_t txl, uint8_t *rx, uint8_t *rxl);
void trf7962a_field(trf7962a_t ctx, bool enabled);
