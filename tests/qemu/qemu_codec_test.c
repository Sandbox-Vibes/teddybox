/* QEMU test of the decoders: decodes the test files with the ADF decoder and
   with the CELT decoder element, PCM checksum (compare with the host decode),
   free internal RAM while decoding, and a jump into the middle of the stream. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "audio_pipeline.h"
#include "audio_element.h"
#include "opus_decoder.h"
#include "tb_opus_dec.h"

static const char *TAG = "CODECTEST";
bool wifi_is_connected(void) { return false; }
extern const uint8_t jingle_start[] asm("_binary_jingle_ogg_start"), jingle_end[] asm("_binary_jingle_ogg_end");
extern const uint8_t music_start[] asm("_binary_music_ogg_start"), music_end[] asm("_binary_music_ogg_end");
static const uint8_t *src, *src_end;
static size_t pos, jump_from, jump_to;
static uint32_t hash, bytes;
static volatile bool finished;

static int rd_cb(audio_element_handle_t el, char *buf, int len, TickType_t t, void *ctx)
{
    if (jump_from && pos >= jump_from)
    {
        pos = jump_to;
        jump_from = 0;
    }
    size_t left = src_end - src - pos;
    if (jump_from && pos + len > jump_from)
    {
        len = jump_from - pos;
    }
    if (!left)
    {
        return AEL_IO_DONE;
    }
    if ((size_t)len > left)
    {
        len = left;
    }
    memcpy(buf, src + pos, len);
    pos += len;
    return len;
}
static int wr_cb(audio_element_handle_t el, char *buf, int len, TickType_t t, void *ctx)
{
    for (int i = 0; i < len; i++)
    {
        hash = (hash ^ (uint8_t)buf[i]) * 16777619u;
    }
    bytes += len;
    return len;
}

static void run(const char *name, bool celt, const uint8_t *s, const uint8_t *e, size_t jf, size_t jt)
{
    audio_pipeline_cfg_t pc = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_handle_t p = audio_pipeline_init(&pc);
    audio_element_handle_t dec;
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (celt)
    {
        dec = tb_opus_dec_init(0, configMAX_PRIORITIES - 1);
    }
    else
    {
        opus_decoder_cfg_t c = DEFAULT_OPUS_DECODER_CONFIG();
        c.stack_in_ext = false;
        c.task_prio = configMAX_PRIORITIES - 1;
        c.out_rb_size = 8 * 1024;
        dec = decoder_opus_init(&c);
    }
    audio_pipeline_register(p, dec, "dec");
    const char *tags[1] = {"dec"};
    audio_pipeline_link(p, &tags[0], 1);
    audio_element_set_read_cb(dec, rd_cb, NULL);
    audio_element_set_write_cb(dec, wr_cb, NULL);
    src = s;
    src_end = e;
    pos = 0;
    jump_from = jf;
    jump_to = jt;
    hash = 2166136261u;
    bytes = 0;
    heap_caps_monitor_local_minimum_free_size_start();
    audio_pipeline_run(p);
    while (audio_element_get_state(dec) == AEL_STATE_RUNNING || bytes == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (audio_element_get_state(dec) >= AEL_STATE_STOPPED && bytes)
        {
            break;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    size_t min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    heap_caps_monitor_local_minimum_free_size_stop();
    ESP_LOGI(TAG, "%-6s %-4s: %u bytes PCM, hash %08x, RAM used while decoding %u bytes (element+task+buffers)", name,
             celt ? "celt" : "adf", (unsigned)bytes, (unsigned)hash, (unsigned)(before - min));
    audio_pipeline_stop(p);
    audio_pipeline_wait_for_stop(p);
    audio_pipeline_terminate(p);
    audio_pipeline_unregister(p, dec);
    audio_pipeline_deinit(p);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    run("jingle", false, jingle_start, jingle_end, 0, 0);
    run("jingle", true, jingle_start, jingle_end, 0, 0);
    run("music", false, music_start, music_end, 0, 0);
    run("music", true, music_start, music_end, 0, 0);
    /* jump: after the headers (first 2 pages) to the page at JUMP_TO */
    run("seek", true, music_start, music_end, JUMP_FROM, JUMP_TO);
    uint64_t us, smp;
    tb_opus_dec_stats(&us, &smp);
    ESP_LOGI(TAG, "celt decode: %llu samples (QEMU timing not meaningful)", (unsigned long long)smp);
    ESP_LOGI(TAG, "done");
    while (1)
    {
        vTaskDelay(1000);
    }
}
