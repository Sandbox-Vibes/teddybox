#pragma once
#include "stubs.h"
typedef struct { int dummy; } audio_hal_codec_config_t;
typedef int audio_hal_codec_mode_t;
typedef int audio_hal_ctrl_t;
typedef struct { int dummy; } audio_hal_codec_i2s_iface_t;
typedef struct {
    esp_err_t (*audio_codec_initialize)(audio_hal_codec_config_t *);
    esp_err_t (*audio_codec_deinitialize)(void);
    esp_err_t (*audio_codec_ctrl)(audio_hal_codec_mode_t, audio_hal_ctrl_t);
    esp_err_t (*audio_codec_config_iface)(audio_hal_codec_mode_t, audio_hal_codec_i2s_iface_t *);
    esp_err_t (*audio_codec_set_mute)(bool);
    esp_err_t (*audio_codec_set_volume)(int);
    esp_err_t (*audio_codec_get_volume)(int *);
} audio_hal_func_t;
