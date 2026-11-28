#pragma once
#include <stdint.h>
void pb_play_content(uint64_t uid);
void pb_play_content_token(uint64_t uid, const uint8_t *token);
void pb_stop(void);
