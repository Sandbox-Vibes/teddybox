#pragma once

#include <stdint.h>

/*
 * Requests from other tasks (MQTT, web, ...) to the main loop. They are
 * applied within ~100 ms by app_main, so only one task talks to the codec
 * and handles power off.
 */

/* volume in percent, clamped to 0..volume_max */
void ctrl_request_volume(int percent);
/* speaker EQ (speaker_eq_t), applied and saved to the INI by the main loop */
void ctrl_request_speaker_eq(int mode);
int ctrl_get_speaker_eq(void);
/* LED brightness 0..3, applied and saved to the INI by the main loop */
void ctrl_request_led_brightness(int level);
int ctrl_get_led_brightness(void);
uint32_t ctrl_get_volume(void);

void ctrl_request_poweroff(void);
