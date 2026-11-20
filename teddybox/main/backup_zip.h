#pragma once
#include <stddef.h>
#include <stdint.h>

/* ZIP archive of up to 8 files without compression ("stored");
   returns its size or -1 if it does not fit into out */
int backup_zip(const char *const names[], const uint8_t *const data[], const size_t sizes[], int count, uint8_t *out,
               size_t out_size);
