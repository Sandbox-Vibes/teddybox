#pragma once

#include <stdint.h>

/*
 * Requests from other tasks (MQTT, web, ...) to the main loop. They are
 * applied within ~100 ms by app_main, so only one task talks to the codec
 * and handles power off.
 */

/* volume in percent, clamped to 0..ctrl_volume_max() */
void ctrl_request_volume(int percent);
/* current limit: volume_max_hdp with headphones plugged in, else volume_max */
int ctrl_volume_max(void);
/* new limits in percent (10..100, -1 = unchanged), saved to the INI by the main loop */
void ctrl_request_volume_max(int speaker, int headphones);
/* speaker EQ (speaker_eq_t), applied and saved to the INI by the main loop */
void ctrl_request_speaker_eq(int mode);
int ctrl_get_speaker_eq(void);
/* LED brightness 0..3, applied and saved to the INI by the main loop */
void ctrl_request_led_brightness(int level);
int ctrl_get_led_brightness(void);
/* slap level 0..5, tilt level 0..5, slap reverse 0/1; -1 = unchanged.
   Takes effect at once, saved to the INI by the main loop */
void ctrl_request_motion(int slap, int tilt, int reverse);
uint32_t ctrl_get_volume(void);

void ctrl_request_poweroff(void);
/* reboot into the original firmware (main loop) */
void ctrl_request_boot_original(void);
/* restart into setup mode (Home Assistant button) */
void ctrl_request_setup(void);
/* sleep timer: minutes 1..240 start it, 0 stops it; see sleep_timer.h */
void ctrl_request_sleep_timer(int minutes);
/* minutes left, 0 = off */
int ctrl_get_sleep_timer(void);
