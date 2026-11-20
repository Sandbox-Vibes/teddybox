#pragma once
/*
 * Audio element: Ogg/Opus (CELT only) decoder with libopus, like the original
 * firmware. Alternative to the ADF decoder (teddybox.ini "decoder = celt").
 */
#include "audio_element.h"

audio_element_handle_t tb_opus_dec_init(int task_core, int task_prio);
/* microseconds spent in opus_decode() and decoded samples since start */
void tb_opus_dec_stats(uint64_t *decode_us, uint64_t *samples);
