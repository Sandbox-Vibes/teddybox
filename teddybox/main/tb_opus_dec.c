/* Ogg/Opus decoder element with the CELT-only libopus, see tb_opus_dec.h */
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "audio_element.h"
#include "audio_mem.h"
#include "opus_rename.h" /* our libopus, not the one in ESP-ADF */
#include "opus.h"
#include "ogg_demux.h"
#include "tb_opus_dec.h"

static const char *TAG = "CELT";

/* 60 ms stereo: the longest packet of a tonie file (teddyCloud encodes 60 ms) */
#define MAX_FRAME 2880
#define IN_BUF 2048
#define TASK_STACK TB_OPUS_TASK_STACK

typedef struct
{
    OpusDecoder *dec;
    ogg_demux_t *demux;
    uint32_t logged_dropped, logged_sync;
    int drop_logs;
    int16_t *pcm;
    int preskip;
    bool info_reported;
    int out_error;
    uint32_t errors;
} tb_opus_t;

static volatile uint64_t stat_us, stat_samples;

void tb_opus_dec_stats(uint64_t *decode_us, uint64_t *samples)
{
    *decode_us = stat_us;
    *samples = stat_samples;
}

static void on_packet(void *ctx, const uint8_t *p, size_t n)
{
    audio_element_handle_t self = (audio_element_handle_t)ctx;
    tb_opus_t *o = (tb_opus_t *)audio_element_getdata(self);
    if (o->out_error)
    {
        return;
    }
    if (n >= 8 && !memcmp(p, "OpusHead", 8))
    {
        /* stream start: samples to drop at the beginning (encoder delay) */
        o->preskip = n >= 12 ? (p[10] | p[11] << 8) : 0;
        opus_decoder_ctl(o->dec, OPUS_RESET_STATE);
        return;
    }
    if (n >= 8 && !memcmp(p, "OpusTags", 8))
    {
        return;
    }
    if (n && (p[0] >> 3) < 16)
    {
        /* SILK or hybrid (low bitrate speech): not part of this decoder */
        if (o->errors++ == 0)
        {
            ESP_LOGW(TAG, "Not a CELT packet: this file needs \"decoder = adf\" in teddybox.ini");
        }
        return;
    }
    int frames = opus_packet_get_nb_samples(p, n, 48000);
    if (frames <= 0 || frames > MAX_FRAME)
    {
        if (o->errors++ < 5)
        {
            ESP_LOGW(TAG, "Skipping packet (%d bytes, %d samples)", (int)n, frames);
        }
        return;
    }
    int64_t t0 = esp_timer_get_time();
    int s = opus_decode(o->dec, p, n, o->pcm, MAX_FRAME, 0);
    stat_us += esp_timer_get_time() - t0;
    if (s < 0)
    {
        if (o->errors++ < 5)
        {
            ESP_LOGW(TAG, "Decoding failed: %s (%d bytes)", opus_strerror(s), (int)n);
        }
        return;
    }
    stat_samples += s;
    if (!o->info_reported)
    {
        audio_element_set_music_info(self, 48000, 2, 16);
        audio_element_report_info(self);
        o->info_reported = true;
    }
    int skip = o->preskip < s ? o->preskip : s;
    o->preskip -= skip;
    int bytes = (s - skip) * 2 * sizeof(int16_t);
    if (bytes > 0)
    {
        int w = audio_element_output(self, (char *)(o->pcm + skip * 2), bytes);
        if (w < 0)
        {
            o->out_error = w;
        }
    }
}

static esp_err_t tb_open(audio_element_handle_t self)
{
    tb_opus_t *o = (tb_opus_t *)audio_element_getdata(self);
    if (!o->dec)
    {
        o->dec = audio_malloc(opus_decoder_get_size(2));
        o->demux = audio_malloc(sizeof(ogg_demux_t));
        o->pcm = audio_malloc(MAX_FRAME * 2 * sizeof(int16_t));
        if (!o->dec || !o->demux || !o->pcm)
        {
            ESP_LOGE(TAG, "No memory for the decoder");
            return ESP_FAIL;
        }
    }
    opus_decoder_init(o->dec, 48000, 2);
    ogg_demux_init(o->demux);
    o->logged_dropped = o->logged_sync = 0;
    o->drop_logs = 0;
    o->preskip = 0;
    o->info_reported = false;
    o->out_error = 0;
    o->errors = 0;
    return ESP_OK;
}

static esp_err_t tb_close(audio_element_handle_t self)
{
    /* the decoder buffers (state, Ogg packet, PCM: about 25 KB) stay from
       the first tonie on: freed and taken again for every file, they had to
       be found again between the pieces a download leaves in the heap */
    if (AEL_STATE_PAUSED != audio_element_get_state(self))
    {
        audio_element_set_byte_pos(self, 0);
    }
    return ESP_OK;
}

static audio_element_err_t tb_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    tb_opus_t *o = (tb_opus_t *)audio_element_getdata(self);
    int r = audio_element_input(self, in_buffer, in_len);
    if (r <= 0)
    {
        return r;
    }
    ogg_demux_feed(o->demux, (const uint8_t *)in_buffer, r, on_packet, self);
    if ((o->demux->dropped != o->logged_dropped || o->demux->sync_lost != o->logged_sync) && o->drop_logs < 20)
    {
        o->drop_logs++;
        ESP_LOGW(TAG, "Ogg: %u packet(s) dropped, %u bytes skipped to resync (since start)",
                 (unsigned)o->demux->dropped, (unsigned)o->demux->sync_lost);
    }
    o->logged_dropped = o->demux->dropped;
    o->logged_sync = o->demux->sync_lost;
    if (o->out_error)
    {
        return o->out_error;
    }
    return r;
}

static esp_err_t tb_destroy(audio_element_handle_t self)
{
    tb_opus_t *o = (tb_opus_t *)audio_element_getdata(self);
    audio_free(o->dec);
    audio_free(o->demux);
    audio_free(o->pcm);
    audio_free(o);
    return ESP_OK;
}

audio_element_handle_t tb_opus_dec_init(int task_core, int task_prio)
{
    tb_opus_t *o = audio_calloc(1, sizeof(tb_opus_t));
    if (!o)
    {
        return NULL;
    }
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = tb_open;
    cfg.close = tb_close;
    cfg.process = tb_process;
    cfg.destroy = tb_destroy;
    cfg.task_stack = TASK_STACK;
    cfg.task_prio = task_prio;
    cfg.task_core = task_core;
    cfg.stack_in_ext = false;
    cfg.buffer_len = IN_BUF;
    cfg.out_rb_size = 8 * 1024;
    cfg.tag = "dec";
    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el)
    {
        audio_free(o);
        return NULL;
    }
    audio_element_setdata(el, o);
    ESP_LOGI(TAG, "CELT decoder (libopus 1.5.2 fixed point, state %d bytes)", opus_decoder_get_size(2));
    return el;
}
