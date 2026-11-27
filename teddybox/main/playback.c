

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"

#include "sdkconfig.h"
#include "audio_element.h"
#include "ringbuf.h"
#include "readahead.h"
#include "taf_verify.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "opus_decoder.h"
#include "i2s_stream.h"
#include "raw_stream.h"

#include "playback.h"
#include "rtnl.h"
#include "taskwatch.h"
#include "nfc.h"
#include "settings.h"

#define PB_DL_CONNECT_TIMEOUT_MS 20000
/* a tonie that needs a download waits this long for WiFi and teddyCloud
   (after power on they need a few seconds) before the network error sound */
#define PB_CLOUD_WAIT_MS 30000
/* the start jingle plays to the end before a tonie starts (as the
   original); at most this long */
#define PB_STARTUP_JINGLE_MAX_MS 10000
#include "dac3100.h"
#include "board.h"
#include "math.h"
#include "ledman.h"
#include "cloud.h"
#include "esp_timer.h"
#include "mempeak.h"
#include "tb_opus_dec.h"
#include "esp_clk_tree.h"
#include "ogg_pos.h"
#include "sd_retry.h"
#include "maint.h"
#include "memdiag.h"

audio_pipeline_handle_t pipeline;
audio_element_handle_t i2s_stream_writer, music_decoder;
audio_event_iface_handle_t evt;

static QueueHandle_t playback_queue;
static bool pb_default_content = false;
static bool pb_playing = false;
/* the start jingle is playing: requests wait for its end */
static bool pb_startup_jingle = false;
static TickType_t pb_startup_since;
static pb_toniefile_t pb_toniefile_info;
static volatile bool pb_headset = false;
static cloud_content_req_t *current_dl_req = NULL;
static int pb_taskwatch = -1;

/* a teddyCloud stream that ended by itself (connection closed: live channel
   switched, teddyCloud restarted, stream_max_size reached) is requested
   again while its tonie stays on the box; streams that end too early in a
   row are given up */
#define PB_STREAM_RESTART_DELAY_MS 1000
#define PB_STREAM_SHORT_BYTES (64 * 1024)
#define PB_STREAM_SHORT_MAX 3
/* teddyCloud encodes files and playlists faster than real time, a radio
   only in real time: when the playback ends this long after the transfer,
   the whole content was there and simply played to the end */
#define PB_STREAM_FINITE_GAP_MS 20000
static uint64_t pb_stream_restart_uid;
static TickType_t pb_stream_restart_at;
static int pb_stream_short;
static uint64_t pb_stream_short_uid;
/* the LED shows "playing download" while the download of the playing tonie runs */
static bool pb_led_download = false;

static bool pb_dl_running(const cloud_content_req_t *dl)
{
    return dl && dl->state != CC_STATE_FINISHED && dl->state != CC_STATE_ERROR && dl->state != CC_STATE_ABORTED;
}

static uint64_t pb_last_nfc_uid = 0;
static uint32_t pb_last_play_position = 0;
/* audio ID of the content the position belongs to (0 = unknown): a new
   episode on the same tonie has another one and starts from the beginning */
static uint32_t pb_last_audio_id = 0;

static const char *TAG = "[PB]";

/* decoder CPU load while playing, first after 10 s, then every 30 s: compares "decoder = adf/celt"
   and "cpu_mhz = 160/240" (FreeRTOS run time of the "dec" task) */
#define PB_LOAD_LOG_MS 30000
static void pb_decoder_load_log(void)
{
    static int64_t t0;
    static uint32_t run0, total0;
    if (!pb_playing || pb_default_content)
    {
        t0 = 0;
        return;
    }
    static int logged;
    int64_t now = esp_timer_get_time();
    if (!t0)
    {
        logged = 0;
    }
    /* first after 10 s (short tests), then every 30 s */
    if (t0 && now - t0 < (logged ? PB_LOAD_LOG_MS : 10000) * 1000LL)
    {
        return;
    }
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = malloc(n * sizeof(TaskStatus_t));
    if (!ts)
    {
        return;
    }
    uint32_t total = 0, run = 0;
    n = uxTaskGetSystemState(ts, n, &total);
    for (UBaseType_t i = 0; i < n; i++)
    {
        if (!strcmp(ts[i].pcTaskName, "dec"))
        {
            run = ts[i].ulRunTimeCounter;
        }
    }
    free(ts);
    if (t0 && total > total0)
    {
        uint32_t mhz = 0;
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &mhz);
        ESP_LOGI(TAG, "Decoder %s at %u MHz: %u.%u %% CPU (of one core)", settings_get()->decoder_celt ? "celt" : "adf",
                 (unsigned)(mhz / 1000000), (unsigned)((uint64_t)(run - run0) * 100 / (total - total0)),
                 (unsigned)((uint64_t)(run - run0) * 1000 / (total - total0) % 10));
        logged++;
    }
    t0 = now;
    run0 = run;
    total0 = total;
}


/* Audio health while playing, logged every 5 s: the rate the I2S output
   really takes (wrong clock: off from 192000 B/s at 48 kHz stereo) and how
   often the PCM buffer before it ran empty (decoder too slow: underruns).
   Only problems are logged; the first 2 s after a start are skipped (the
   clock change at the start pauses the output briefly). */
#define PB_RATE_CHECK_MS 5000
static volatile int pb_prog_total = -1, pb_prog_chapter = -1, pb_prog_pos_ds = -1, pb_prog_dur_ds = -1, pb_prog_dl = -1;

/* glitch monitor: every 5 ms while playing, catches gaps far shorter than the
   5 s window of the rate check. Empty PCM buffer = the decoder was late;
   I2S position standing still with data waiting = the output was late. */
#define PB_MON_US 5000
#define PB_MON_EMPTY_MS 15
#define PB_MON_STALL_MS 60
static esp_timer_handle_t pb_mon_timer;
static volatile bool pb_i2s_paused;
static volatile uint32_t pb_mon_empty, pb_mon_empty_max, pb_mon_stall, pb_mon_stall_max;
/* what the output and the cores did when the output stood still the first time in a window */
static char pb_mon_detail[96];

static const char *pb_task_state(const char *name)
{
    TaskHandle_t t = xTaskGetHandle(name);
    if (!t)
    {
        return "-";
    }
    switch (eTaskGetState(t))
    {
    case eRunning:
        return "running";
    case eReady:
        return "ready";
    case eBlocked:
        return "blocked";
    case eSuspended:
        return "suspended";
    default:
        return "?";
    }
}

static void pb_mon_cb(void *arg)
{
    static int empty_run, stall_run;
    static int64_t last_pos, since;
    if (!pb_playing || pb_i2s_paused || !i2s_stream_writer)
    {
        empty_run = stall_run = 0;
        since = 0;
        return;
    }
    /* the start (and a restart after a pause) has its own short stops */
    int64_t now = esp_timer_get_time();
    if (!since)
    {
        since = now;
    }
    if (now - since < 2000000)
    {
        empty_run = stall_run = 0;
        return;
    }
    ringbuf_handle_t rb = audio_element_get_input_ringbuf(i2s_stream_writer);
    if (!rb)
    {
        return;
    }
    int fill = rb_bytes_filled(rb);
    audio_element_info_t info = {0};
    audio_element_getinfo(i2s_stream_writer, &info);

    if (fill < 512)
    {
        empty_run++;
    }
    else
    {
        uint32_t ms = empty_run * PB_MON_US / 1000;
        if (ms >= PB_MON_EMPTY_MS)
        {
            pb_mon_empty++;
            if (ms > pb_mon_empty_max)
            {
                pb_mon_empty_max = ms;
            }
        }
        empty_run = 0;
    }

    if (info.byte_pos == last_pos && fill >= 4096)
    {
        stall_run++;
        if (stall_run == PB_MON_STALL_MS * 1000 / PB_MON_US && !pb_mon_detail[0])
        {
            TaskHandle_t c0 = xTaskGetCurrentTaskHandleForCore(0), c1 = xTaskGetCurrentTaskHandleForCore(1);
            snprintf(pb_mon_detail, sizeof(pb_mon_detail), "i2s %s, dec %s, core0 %s, core1 %s, PCM %d B",
                     pb_task_state("i2s"), pb_task_state("dec"), c0 ? pcTaskGetName(c0) : "-",
                     c1 ? pcTaskGetName(c1) : "-", fill);
        }
    }
    else
    {
        uint32_t ms = stall_run * PB_MON_US / 1000;
        if (ms >= PB_MON_STALL_MS)
        {
            pb_mon_stall++;
            if (ms > pb_mon_stall_max)
            {
                pb_mon_stall_max = ms;
            }
        }
        stall_run = 0;
    }
    last_pos = info.byte_pos;
}

static void pb_mon_start(void)
{
    if (pb_mon_timer)
    {
        return;
    }
    const esp_timer_create_args_t a = {.callback = pb_mon_cb, .name = "pb_mon"};
    if (esp_timer_create(&a, &pb_mon_timer) == ESP_OK)
    {
        esp_timer_start_periodic(pb_mon_timer, PB_MON_US);
    }
}

static void pb_audio_rate_check(void)
{
    static int64_t t0 = 0, started = 0;
    static int64_t pos0 = 0;
    static int empty = 0, samples = 0;
    if (!pb_playing || !i2s_stream_writer)
    {
        t0 = started = 0;
        return;
    }
    if (!started)
    {
        started = esp_timer_get_time();
    }
    if (esp_timer_get_time() - started < 2000000)
    {
        return;
    }
    ringbuf_handle_t rb = audio_element_get_input_ringbuf(i2s_stream_writer);
    if (rb)
    {
        samples++;
        empty += rb_bytes_filled(rb) < 512;
    }
    audio_element_info_t info = {0};
    audio_element_getinfo(i2s_stream_writer, &info);
    int64_t now = esp_timer_get_time();
    if (!t0 || info.byte_pos < pos0)
    {
        t0 = now;
        pos0 = info.byte_pos;
        empty = samples = 0;
        return;
    }
    if (now - t0 < PB_RATE_CHECK_MS * 1000LL)
    {
        return;
    }
    int rate = (int)((info.byte_pos - pos0) * 1000000LL / (now - t0));
    int expected = info.sample_rates * info.channels * info.bits / 8;
    int pct = expected ? rate * 100 / expected : 0;
    uint32_t me = pb_mon_empty, mem = pb_mon_empty_max, ms = pb_mon_stall, msm = pb_mon_stall_max;
    pb_mon_empty = pb_mon_empty_max = pb_mon_stall = pb_mon_stall_max = 0;
    /* (cleared after the log line below) */
    if (me || ms)
    {
        int32_t sec = pb_prog_pos_ds > 0 ? pb_prog_pos_ds / 10 : 0;
        ESP_LOGW(TAG, "Audio gap near %d:%02d: decoder late %u x (longest %u ms), output late %u x (longest %u ms)%s%s",
                 (int)(sec / 60), (int)(sec % 60), (unsigned)me, (unsigned)mem, (unsigned)ms, (unsigned)msm,
                 pb_mon_detail[0] ? "; at the first: " : "", pb_mon_detail);
    }
    pb_mon_detail[0] = 0;
    if (pct < 97 || pct > 103 || empty * 10 > samples)
    {
        ESP_LOGW(TAG, "Audio: %d B/s to I2S (%d %% of %d Hz/%d ch), PCM buffer empty %d of %d times", rate, pct,
                 info.sample_rates, info.channels, empty, samples);
    }
    else
    {
        ESP_LOGD(TAG, "Audio: ok (%d %%, buffer empty %d/%d)", pct, empty, samples);
    }
    t0 = now;
    pos0 = info.byte_pos;
    empty = samples = 0;
}


/* ********************************************************************************************************************* */
/* ********************************************************************************************************************* */
/* ********************************************************************************************************************* */

TonieboxAudioFileHeader *pb_toniefile_get_header(FILE *fd)
{
    fseek(fd, 0, SEEK_SET);

    uint8_t proto_be[4];
    if (fread(proto_be, 4, 1, fd) != 1)
    {
        ESP_LOGE(TAG, "Failed to read header size");
        return NULL;
    }
    uint32_t proto_size = (proto_be[0] << 24) | (proto_be[1] << 16) | (proto_be[2] << 8) | proto_be[3];
    if (proto_size > TONIEFILE_FRAME_SIZE)
    {
        ESP_LOGE(TAG, "Invalid header size: 0x%04X", proto_size);
        return NULL;
    }
    uint8_t *buffer = malloc(proto_size);
    if (!buffer)
    {
        ESP_LOGE(TAG, "No memory for the header");
        return NULL;
    }
    if (fread(buffer, proto_size, 1, fd) != 1)
    {
        free(buffer);
        ESP_LOGE(TAG, "Failed to read header");
        return NULL;
    }

    TonieboxAudioFileHeader *taf = toniebox_audio_file_header__unpack(NULL, proto_size, (const uint8_t *)buffer);
    free(buffer);
    if (!taf)
    {
        ESP_LOGE(TAG, "Failed to parse header");
        return NULL;
    }

    return taf;
}

/* audio ID from a TAF header on the card, 0 if there is none (yet) */
static uint32_t pb_file_audio_id(const char *path)
{
    if (!path)
    {
        return 0;
    }
    FILE *fd = fopen(path, "rb");
    if (!fd)
    {
        return 0;
    }
    uint32_t id = 0;
    TonieboxAudioFileHeader *taf = pb_toniefile_get_header(fd);
    if (taf)
    {
        id = taf->audio_id;
        toniebox_audio_file_header__free_unpacked(taf, NULL);
    }
    fclose(fd);
    return id;
}

esp_err_t pb_toniefile_open(pb_toniefile_t *info, const char *filepath)
{
    memset(info, 0x00, sizeof(pb_toniefile_t));

    /* while downloading, the data is in the .tmp file (reopened after an SD error) */
    info->filename = strdup(current_dl_req ? current_dl_req->tmpname : filepath);

    if (current_dl_req)
    {
        ESP_LOGI(TAG, "Download in progress, using already open file");
        while (current_dl_req->state < CC_STATE_RECEIVING)
        {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        while (!xSemaphoreTake(current_dl_req->file_sem, 1000 / portTICK_PERIOD_MS))
        {
            ESP_LOGE(TAG, "Open: Timed out waiting for file lock...");
        }
        info->taf = pb_toniefile_get_header(current_dl_req->handle);
        xSemaphoreGive(current_dl_req->file_sem);
        if (!info->taf)
        {
            free(info->filename);
            return ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGI(TAG, "Open Toniefile: %s", info->filename);
        info->fd = fopen(info->filename, "rb");
        if (!info->fd)
        {
            ESP_LOGE(TAG, "Failed to read file: %s", info->filename);
            free(info->filename);
            return ESP_FAIL;
        }
        info->taf = pb_toniefile_get_header(info->fd);
        if (!info->taf)
        {
            fclose(info->fd);
            free(info->filename);
            return ESP_FAIL;
        }
        /* the block buffer is free until playback starts */
        info->total_granule = pb_ogg_total_granule(info->fd, info->current_block_buffer, sizeof(info->current_block_buffer));
    }

    ESP_LOGI(TAG, "  Audio ID: %08X", info->taf->audio_id);
    ESP_LOGI(TAG, "  Size:     %08llX", info->taf->num_bytes);
    ESP_LOGI(TAG, "  Chapters: %d", info->taf->n_track_page_nums);
    for (int chap = 0; chap < info->taf->n_track_page_nums; chap++)
    {
        ESP_LOGI(TAG, "    %d: offset %08X", chap, info->taf->track_page_nums[chap]);
    }

    info->current_pos = TONIEFILE_FRAME_SIZE;
    info->current_block = 0;
    info->current_chapter = 0;
    info->target_chapter = -1;
    info->target_pos = -1;
    info->seek_blocks = 0;
    info->valid = true;
    info->initialized = false;

    return ESP_OK;
}

void pb_toniefile_close(pb_toniefile_t *info)
{
    if (!info->valid)
    {
        return;
    }

    if (info->ra)
    {
        uint32_t hits, misses;
        readahead_stats(info->ra, &hits, &misses);
        ESP_LOGI(TAG, "Read-ahead: %u blocks from the buffer, %u read directly", (unsigned)hits, (unsigned)misses);
        readahead_destroy(info->ra);
        info->ra = NULL;
    }

    FILE *fd = info->fd;
    info->valid = false;
    info->fd = NULL;
    /* the download's handle is closed with the download; an own read
       handle (opened after an SD error) always here */
    if (fd)
    {
        fclose(fd);
    }
    toniebox_audio_file_header__free_unpacked(info->taf, NULL);
    info->taf = NULL;
    if (info->filename)
    {
        /* a file found damaged meanwhile is renamed now */
        taf_verify_file_closed(info->filename);
    }
    free(info->filename);
    info->filename = NULL;
}

/* progress for MQTT: written by the playback side only, read as plain ints */

static void pb_progress_reset(void)
{
    pb_prog_total = pb_prog_chapter = pb_prog_pos_ds = pb_prog_dur_ds = -1;
}

void pb_get_progress(pb_progress_t *p)
{
    p->progress = pb_prog_total;
    p->chapter_progress = pb_prog_chapter;
    p->position_ds = pb_prog_pos_ds;
    p->duration_ds = pb_prog_dur_ds;
    p->download = pb_prog_dl;
}

static int pb_percent(int64_t part, int64_t whole)
{
    if (whole <= 0)
    {
        return -1;
    }
    int64_t p = part * 100 / whole;
    return p < 0 ? 0 : (p > 100 ? 100 : (int)p);
}

/* waiting for data of a running download */
#define PB_DL_WAIT_STEP_MS 50
#define PB_DL_WAIT_MAX_MS 3000
#define PB_LOCK_WAIT_WARN_US 100000

/* reads one block. "shared" is the download's write handle (or NULL); "own"
   is a read-only handle of the playback. On an SD error (FATFS then refuses
   every further access to that file) a read-only handle is opened and the
   read retried. Never the shared handle: reopened for writing, seeking
   beyond the size in the directory entry would extend the file. */
static int pb_read_block(FILE *shared, FILE **own, const char *filename, long offset, uint8_t *buf, int want)
{
    for (int attempt = 0; attempt < SD_RETRIES; attempt++)
    {
        if (attempt)
        {
            ESP_LOGW(TAG, "SD read at %ld failed, reopening the file (attempt %d)", offset, attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(sd_retry_delay_ms(attempt)));
        }
        if (attempt || (!*own && !shared))
        {
            if (*own)
            {
                fclose(*own);
            }
            *own = fopen(filename, "rb");
        }
        FILE *fd = *own ? *own : shared;
        if (fd && fseek(fd, offset, SEEK_SET) == 0)
        {
            int n = fread(buf, 1, want, fd);
            /* short without error: end of the data written so far */
            if (n == want || !ferror(fd))
            {
                if (attempt)
                {
                    ESP_LOGI(TAG, "SD read at %ld worked again", offset);
                }
                return n;
            }
        }
    }
    return 0;
}

static size_t pb_ensure_block(pb_toniefile_t *info, FILE *shared)
{
    bool read = false;

    /* when starting with no block preselected, we can safely start playing at the first block */
    if (info->current_block == 0)
    {
        info->initialized = true;
    }

    /* else, before we can play any position, read the initial ogg header block */
    if (!info->initialized)
    {
        if (info->current_pos == TONIEFILE_FRAME_SIZE)
        {
            info->current_block_avail = pb_read_block(shared, &info->fd, info->filename, TONIEFILE_FRAME_SIZE,
                                                      info->current_block_buffer, 0x200);
        }

        /* before we continue, first consume the initial Ogg block */
        int32_t offset = info->current_pos % TONIEFILE_FRAME_SIZE;
        if (info->current_block_avail > offset)
        {
            return info->current_block_avail - offset;
        }

        /* consumed the 0x200 bytes "first-block", proceed to normal operation */
        info->current_pos = info->current_block * TONIEFILE_FRAME_SIZE;
        info->current_block = 0;
        info->initialized = true;
    }

    if (info->current_pos >= (info->current_block + 1) * TONIEFILE_FRAME_SIZE)
    {
        read = true;
    }
    if (info->current_pos < info->current_block * TONIEFILE_FRAME_SIZE)
    {
        read = true;
    }
    /* a block that was not complete yet (download still running): read it again */
    if (info->current_block_avail < TONIEFILE_FRAME_SIZE &&
        (info->current_pos % TONIEFILE_FRAME_SIZE) >= info->current_block_avail)
    {
        read = true;
    }

    if (read)
    {
        info->current_block = info->current_pos / TONIEFILE_FRAME_SIZE;
        info->current_block_avail = pb_read_block(shared, &info->fd, info->filename, info->current_block * TONIEFILE_FRAME_SIZE,
                                                  info->current_block_buffer, TONIEFILE_FRAME_SIZE);
        /* every TAF block starts with an Ogg page */
        uint64_t g = pb_ogg_granule(info->current_block_buffer, info->current_block_avail);
        if (g != PB_NO_GRANULE)
        {
            info->granule = g;
        }
    }
    if (!pb_default_content)
    {
        pb_last_play_position = info->current_block;
    }

    /* nothing (more) in this block: 0, never a wrapped around size_t */
    int32_t offset = info->current_pos % TONIEFILE_FRAME_SIZE;
    return info->current_block_avail > offset ? (size_t)(info->current_block_avail - offset) : 0;
}

/* read-ahead: 8 blocks = 32 KiB, about 2.7 s at 96 kbit/s */
#define PB_READAHEAD_BLOCKS 8
#define PB_READAHEAD_PRIO 6

typedef struct
{
    char *filename;
    FILE *fd;
} pb_ra_ctx_t;

/* in the read-ahead task: own read-only handle, retried like the playback's */
static int pb_ra_read(void *ctx, uint32_t block, uint8_t *buf)
{
    pb_ra_ctx_t *c = ctx;
    int n = pb_read_block(NULL, &c->fd, c->filename, (long)block * TONIEFILE_FRAME_SIZE, buf, TONIEFILE_FRAME_SIZE);
    if (n < TONIEFILE_FRAME_SIZE && c->fd)
    {
        /* a read handle knows only the size at its opening: open again next
           time to see what the download wrote meanwhile */
        fclose(c->fd);
        c->fd = NULL;
    }
    return n;
}

static void pb_ra_free(void *ctx)
{
    pb_ra_ctx_t *c = ctx;
    if (c->fd)
    {
        fclose(c->fd);
    }
    free(c->filename);
    free(c);
}

/* the current position from the block buffer or the read-ahead, without the
   download's file lock (the download may hold it for seconds while the SD
   card hangs). 0: read it the normal way. */
static int pb_try_buffered(pb_toniefile_t *info)
{
    if (!info->initialized || pb_default_content)
    {
        return 0;
    }
    int32_t block = info->current_pos / TONIEFILE_FRAME_SIZE;
    int32_t offset = info->current_pos % TONIEFILE_FRAME_SIZE;
    if (block == info->current_block && offset < info->current_block_avail)
    {
        return info->current_block_avail - offset;
    }

    if (!info->ra && info->taf && settings_get()->readahead)
    {
        pb_ra_ctx_t *c = calloc(1, sizeof(*c));
        if (c)
        {
            c->filename = strdup(info->filename);
            uint32_t limit = 1 + (info->taf->num_bytes + TONIEFILE_FRAME_SIZE - 1) / TONIEFILE_FRAME_SIZE;
            info->ra = readahead_create(PB_READAHEAD_BLOCKS, TONIEFILE_FRAME_SIZE, limit, pb_ra_read, c, pb_ra_free,
                                        PB_READAHEAD_PRIO, tskNO_AFFINITY);
        }
    }
    if (!info->ra || !readahead_get(info->ra, block, info->current_block_buffer))
    {
        return 0;
    }
    info->current_block = block;
    info->current_block_avail = TONIEFILE_FRAME_SIZE;
    uint64_t g = pb_ogg_granule(info->current_block_buffer, info->current_block_avail);
    if (g != PB_NO_GRANULE)
    {
        info->granule = g;
    }
    pb_last_play_position = info->current_block;
    return TONIEFILE_FRAME_SIZE - offset;
}

int pb_toniefile_cbr(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    pb_toniefile_t *info = (pb_toniefile_t *)context;

    if (!info || !info->valid)
    {
        ESP_LOGE(TAG, "Playback already finished, but was called again");
        return AEL_IO_DONE;
    }

    /* doing seeking here, saves us some semaphores*/
    if (info->target_chapter >= 0)
    {
        ESP_LOGI(TAG, "Set target chapter %d", info->target_chapter);
        if (info->target_chapter < info->taf->n_track_page_nums)
        {
            uint32_t block = 1 + info->taf->track_page_nums[info->target_chapter];
            uint32_t offset = block * TONIEFILE_FRAME_SIZE;

            ESP_LOGI(TAG, " -> block %d, offset %d", block, offset);
            info->target_pos = offset;
        }
        info->target_chapter = -1;
    }

    if (info->seek_blocks)
    {
        int32_t skip_bytes = info->seek_blocks * TONIEFILE_FRAME_SIZE;

        if (info->current_pos + skip_bytes < 0)
        {
            info->target_pos = 0;
        }
        else
        {
            info->target_pos = info->current_pos + skip_bytes;
        }
        ESP_LOGI(TAG, "Seek %d blocks, current offset 0x%08X, target 0x%08X", info->seek_blocks, info->current_pos, info->target_pos);
        info->seek_blocks = 0;
    }

    /* operations above want us to seek to a certain file offset. do that here */
    if (info->target_pos >= 0)
    {
        /* do not seek to the stream start, else the decoder sees a stream start packet which causes errors */
        if (info->target_pos < 2 * TONIEFILE_FRAME_SIZE)
        {
            info->target_pos = 2 * TONIEFILE_FRAME_SIZE;
        }

        /* before seeking, wait until we are block-aligned again */
        if ((info->current_pos % TONIEFILE_FRAME_SIZE) == 0)
        {
            ESP_LOGI(TAG, "Seeking possible");
            info->current_pos = info->target_pos;
            info->target_pos = -1;
        }
        else
        {
            /* not yet, only read up to block end */
            uint32_t max_len = TONIEFILE_FRAME_SIZE - (info->current_pos % TONIEFILE_FRAME_SIZE);

            if (len > max_len)
            {
                ESP_LOGI(TAG, "Seeking delayed, only reading %d bytes to match block size", len);
                len = max_len;
            }
        }
    }

    /* update chapter info */
    {
        int32_t current_block = (info->current_pos / TONIEFILE_FRAME_SIZE) - 1;

        /* the chapter is the last one that starts at or before this block
           (the old loop never reached the last chapter) */
        int found = 0;
        for (int chap = 0; chap < info->taf->n_track_page_nums; chap++)
        {
            if (current_block >= (int32_t)info->taf->track_page_nums[chap])
            {
                found = chap;
            }
        }
        if (info->current_chapter != found)
        {
            info->current_chapter = found;
            ESP_LOGI(TAG, "Current chapter: %d", info->current_chapter);
        }
    }

    /* progress: by time if the duration is known, else by position in the file */
    if (!pb_default_content)
    {
        int32_t audio_blocks = info->taf->num_bytes / TONIEFILE_FRAME_SIZE;
        int32_t block = (info->current_pos / TONIEFILE_FRAME_SIZE) - 1;
        if (block < 0)
        {
            block = 0;
        }
        pb_prog_pos_ds = info->granule ? (int)(info->granule * 10 / PB_OPUS_RATE) : 0;
        pb_prog_dur_ds = info->total_granule ? (int)(info->total_granule * 10 / PB_OPUS_RATE) : -1;
        pb_prog_total = info->total_granule ? pb_percent(info->granule, info->total_granule) : pb_percent(block, audio_blocks);
        int chap = info->current_chapter;
        int n = info->taf->n_track_page_nums;
        if (chap >= 0 && chap < n)
        {
            int32_t start = info->taf->track_page_nums[chap];
            int32_t end = chap + 1 < n ? (int32_t)info->taf->track_page_nums[chap + 1] : audio_blocks;
            pb_prog_chapter = pb_percent(block - start, end - start);
        }
        else
        {
            pb_prog_chapter = -1;
        }
    }

    int bytes_read = 0;
    int avail = pb_try_buffered(info);
    if (avail > 0)
    {
        /* from the block buffer or the read-ahead */
    }
    else if (current_dl_req)
    {
        /* while the download runs, missing data means "not yet", not "end";
           wait a while for it (bounded, so a stop request is not held up) */
        cloud_content_req_t *dl = current_dl_req;
        for (int waited = 0;; waited += PB_DL_WAIT_STEP_MS)
        {
            int64_t t0 = esp_timer_get_time();
            while (!xSemaphoreTake(dl->file_sem, 1000 / portTICK_PERIOD_MS))
            {
                ESP_LOGE(TAG, "Playback: Timed out waiting for file lock...");
            }
            int64_t wait_us = esp_timer_get_time() - t0;
            if (wait_us > PB_LOCK_WAIT_WARN_US)
            {
                ESP_LOGW(TAG, "Waited %d ms for the download's file lock", (int)(wait_us / 1000));
            }
            avail = pb_ensure_block(info, dl->handle);
            /* also while the cloud task resumes a lost connection */
            bool more_coming = pb_dl_running(dl) && dl->content_length && !dl->abort &&
                               (uint32_t)info->current_pos < dl->content_length;
            if (avail <= 0 && more_coming && info->fd)
            {
                /* the own read handle (after an SD error) only knows the size
                   at its opening; back to the download's handle */
                fclose(info->fd);
                info->fd = NULL;
            }
            xSemaphoreGive(dl->file_sem);
            if (avail <= 0 && more_coming && waited >= PB_DL_WAIT_MAX_MS)
            {
                /* not the end: the download is still coming (e.g. resuming);
                   the decoder calls again, a stop request is not held up */
                return AEL_IO_TIMEOUT;
            }
            if (avail > 0 || !more_coming)
            {
                break;
            }
            if (!waited)
            {
                ESP_LOGW(TAG, "Playback caught up with the download (%u of %u), waiting", (unsigned)dl->received,
                         (unsigned)dl->content_length);
            }
            vTaskDelay(pdMS_TO_TICKS(PB_DL_WAIT_STEP_MS));
        }
    }
    else
    {
        avail = pb_ensure_block(info, NULL);
    }

    if (len > avail)
    {
        len = avail;
    }
    int offset = info->current_pos % TONIEFILE_FRAME_SIZE;
    memcpy(buffer, &info->current_block_buffer[offset], len);
    bytes_read = len;

    if (bytes_read > 0)
    {
        info->current_pos += bytes_read;
    }
    else if (bytes_read == 0)
    {
        ESP_LOGI(TAG, "Playback finished, cleaning up");
        /* played to the very end: next time the tonie starts from the
           beginning, like the original firmware. Not after a download that
           stopped halfway, there the position is kept for the resume. */
        if (!pb_default_content && info->taf && (uint64_t)info->current_pos >= info->taf->num_bytes + TONIEFILE_FRAME_SIZE)
        {
            ESP_LOGI(TAG, "Tonie played to the end, next time from the start");
            pb_last_play_position = 0;
        }
        pb_toniefile_close(info);
        return AEL_IO_DONE;
    }
    else
    {
        ESP_LOGI(TAG, "Reading failed, cleaning up");
        pb_toniefile_close(info);
        return AEL_IO_DONE;
    }

    return bytes_read;
}

esp_err_t pb_seek(int32_t blocks)
{
    if (!pb_toniefile_info.valid)
    {
        return ESP_FAIL;
    }
    pb_toniefile_info.seek_blocks += blocks;
    return ESP_OK;
}

esp_err_t pb_set_chapter(int32_t chapter)
{
    if (!pb_toniefile_info.valid)
    {
        return ESP_FAIL;
    }
    pb_toniefile_info.target_chapter = chapter;
    return ESP_OK;
}

bool pb_headphones_connected()
{
    return pb_headset;
}

int32_t pb_get_chapter_count()
{
    TonieboxAudioFileHeader *taf = pb_toniefile_info.taf;
    if (!pb_toniefile_info.valid || !taf || pb_default_content)
    {
        return 0;
    }
    return taf->n_track_page_nums;
}

int32_t pb_get_chapter()
{
    if (!pb_toniefile_info.valid)
    {
        return ESP_FAIL;
    }
    return pb_toniefile_info.current_chapter;
}

esp_err_t pb_seek_chapter(int32_t chapters)
{
    if (!pb_toniefile_info.valid)
    {
        return ESP_FAIL;
    }
    int32_t target = pb_get_chapter() + chapters;

    if (target < 0)
    {
        target = 0;
    }
    if (target >= pb_toniefile_info.taf->n_track_page_nums)
    {
        target = pb_toniefile_info.taf->n_track_page_nums - 1;
    }
    return pb_set_chapter(target);
}

/* ********************************************************************************************************************* */
/* ********************************************************************************************************************* */
/* ********************************************************************************************************************* */

char *pb_build_filename(uint64_t id)
{
    char *filename = malloc(48);
    char *filename_ptr = filename;

    filename_ptr += sprintf(filename_ptr, "/sdcard/CONTENT/");

    for (int i = 0; i < 4; ++i)
    {
        uint8_t byte = (id >> (i * 8)) & 0xFF;
        filename_ptr += sprintf(filename_ptr, "%02X", byte);
    }
    filename_ptr += sprintf(filename_ptr, "/");
    for (int i = 4; i < 8; ++i)
    {
        uint8_t byte = (id >> (i * 8)) & 0xFF;
        filename_ptr += sprintf(filename_ptr, "%02X", byte);
    }
    return filename;
}

esp_err_t pb_play(const char *uri)
{
    ESP_LOGI(TAG, "[ * ] Queue uri: '%s'", uri);
    char *msg = strdup(uri);
    xQueueSend(playback_queue, &msg, portMAX_DELAY);

    return ESP_OK;
}

bool pb_file_is_own(const char *filename)
{
    char own[72];
    snprintf(own, sizeof(own), "%s.own", filename);
    struct stat st;
    return stat(own, &st) == 0;
}

bool pb_file_is_stream(const char *filename)
{
    FILE *fd = fopen(filename, "rb");
    if (!fd)
    {
        return false;
    }
    TonieboxAudioFileHeader *taf = pb_toniefile_get_header(fd);
    struct stat st;
    bool have_size = fstat(fileno(fd), &st) == 0;
    fclose(fd);
    /* a complete file is a tonie, whatever its length: real TAF files can
       end with a short page (teddyCloud stream files never complete) */
    bool stream = taf && PB_IS_STREAM_LENGTH(taf->num_bytes) &&
                  !(have_size && (uint64_t)st.st_size >= (uint64_t)taf->num_bytes + TONIEFILE_FRAME_SIZE);
    if (taf)
    {
        toniebox_audio_file_header__free_unpacked(taf, NULL);
    }
    return stream;
}

bool pb_finish_tmp(const char *filename)
{
    char tmp[64];
    struct stat st;
    snprintf(tmp, sizeof(tmp), "%s.tmp", filename);
    if (stat(tmp, &st) != 0 || pb_check_file(tmp) != PB_ERR_GOOD_FILE)
    {
        return false;
    }
    unlink(filename);
    if (rename(tmp, filename) != 0)
    {
        ESP_LOGE(TAG, "Could not rename '%s' to '%s'", tmp, filename);
        return false;
    }
    ESP_LOGI(TAG, "Download complete: '%s'", filename);
    /* check the new file once in the background */
    FILE *fd = fopen(filename, "rb");
    if (fd)
    {
        TonieboxAudioFileHeader *taf = pb_toniefile_get_header(fd);
        fclose(fd);
        if (taf)
        {
            taf_verify_start(filename, taf->sha1_hash.data, taf->sha1_hash.len, taf->num_bytes, taf->audio_id);
            toniebox_audio_file_header__free_unpacked(taf, NULL);
        }
    }
    return true;
}

esp_err_t pb_check_file(const char *filename)
{
    ESP_LOGI(TAG, "Check file '%s'", filename);

    /* no file at all, we have to download it */
    struct stat st;
    if (stat(filename, &st) != 0)
    {
        ESP_LOGE(TAG, "does not exist: '%s'", filename);
        return PB_ERR_NO_FILE;
    }

    if (st.st_size < TONIEFILE_FRAME_SIZE)
    {
        ESP_LOGE(TAG, "file size: %ld", st.st_size);
        return PB_ERR_EMPTY_FILE;
    }

    /* read details */
    FILE *fd = fopen(filename, "rb");
    if (!fd)
    {
        ESP_LOGE(TAG, "Failed to open file: '%s'", filename);
        return PB_ERR_NO_FILE;
    }

    TonieboxAudioFileHeader *taf = pb_toniefile_get_header(fd);
    fclose(fd);

    if (!taf)
    {
        ESP_LOGE(TAG, "Failed to read file: '%s'", filename);
        return PB_ERR_CORRUPTED_FILE;
    }

    esp_err_t ret = PB_ERR_GOOD_FILE;

    if (st.st_size != taf->num_bytes + TONIEFILE_FRAME_SIZE)
    {
        ESP_LOGW(TAG, "  TAF size: %llu, file size: %ld -> partial", taf->num_bytes, st.st_size);
        ret = PB_ERR_PARTIAL_FILE;
    }
    toniebox_audio_file_header__free_unpacked(taf, NULL);

    return ret;
}

esp_err_t pb_play_default(uint32_t id)
{
    pb_req_default_t *req = malloc(sizeof(pb_req_default_t));

    req->hdr.type = PB_REQ_TYPE_DEFAULT;
    req->voiceline = id;

    xQueueSend(playback_queue, &req, portMAX_DELAY);
    return ESP_OK;
}

/* when being called with the token, it depends on the play state what to do */
esp_err_t pb_play_content_token(uint64_t nfc_uid, const uint8_t *token)
{
    pb_req_play_token_t *req = malloc(sizeof(pb_req_play_token_t));

    req->hdr.type = PB_REQ_TYPE_PLAY_TOKEN;
    req->uid = nfc_uid;
    memcpy(req->token, token, 32);
    /* for the nightly update, which downloads without the tonie on the box */
    maint_token_save(nfc_uid, token);
    cloud_claim(nfc_uid, token);
    xQueueSend(playback_queue, &req, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t pb_play_content(uint64_t nfc_uid)
{
    pb_req_play_t *req = malloc(sizeof(pb_req_play_t));

    req->hdr.type = PB_REQ_TYPE_PLAY;
    req->uid = nfc_uid;

    xQueueSend(playback_queue, &req, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t pb_stop()
{
    pb_req_stop_t *req = malloc(sizeof(pb_req_stop_t));

    req->hdr.type = PB_REQ_TYPE_STOP;

    xQueueSend(playback_queue, &req, portMAX_DELAY);
    return ESP_OK;
}

bool pb_is_playing()
{
    return pb_playing;
}

uint32_t pb_get_play_position()
{
    if (!pb_is_playing())
    {
        return 0;
    }

    return pb_toniefile_info.current_block;
}

uint32_t pb_get_resume_position()
{
    return pb_last_play_position;
}

uint32_t pb_get_resume_audio_id(void)
{
    return pb_last_audio_id;
}

uint64_t pb_get_current_uid()
{
    return pb_last_nfc_uid;
}

void pb_set_last(uint64_t nfc_uid, uint32_t play_position, uint32_t audio_id)
{
    pb_last_nfc_uid = nfc_uid;
    pb_last_play_position = play_position;
    pb_last_audio_id = audio_id;
}

/********************************************************/
/* helpers for main loop, only to be called from there  */
/********************************************************/

/* the decoder start is one of the two big memory peaks, see mempeak.h */
/* The decoder task is created anew for every playback (ADF elements end
   their task on terminate) and needs one free block of its stack size
   (18 KB). After downloads and TLS handshakes the heap can be cut up so
   that no block is that large any more (27 Nov: "allocation of 22528
   bytes failed", playback silent and stuck). So a block of that size is
   kept while nothing plays and given back right before the pipeline
   starts: the task gets that very hole. */
static void *pb_dec_reserve;
static TickType_t pb_idle_since;

static void pb_dec_reserve_take(void)
{
    if (pb_dec_reserve || !settings_get()->decoder_celt)
    {
        return;
    }
    /* only when such a block is there: a failed malloc would log */
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= TB_OPUS_TASK_STACK + 64)
    {
        pb_dec_reserve = heap_caps_malloc(TB_OPUS_TASK_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
}

static void pb_dec_reserve_give(void)
{
    free(pb_dec_reserve);
    pb_dec_reserve = NULL;
}

static bool pb_mempeak_taken = false;
static TickType_t pb_mempeak_since = 0;
#define PB_MEMPEAK_MAX_MS 3000

static void pb_mempeak_release(void)
{
    if (pb_mempeak_taken)
    {
        pb_mempeak_taken = false;
        mempeak_leave(true);
    }
}

static esp_err_t pb_int_abort_dl()
{
    /* do a semi-atomic swap, so other threads do not interfere */
    cloud_content_req_t *curr = current_dl_req;
    current_dl_req = NULL;

    if (curr)
    {
        ESP_LOGI(TAG, "There is a download for '%s'", curr->filename);
        curr->abort = true;
        while (!curr->done)
        {
            ESP_LOGI(TAG, "wait for abort...");
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        if (curr->handle)
        {
            fclose(curr->handle);
            curr->handle = NULL;
        }
        cloud_content_cleanup(curr);
    }

    return ESP_OK;
}

static void pb_int_stop()
{
    if (!pb_playing)
    {
        return;
    }
    audio_pipeline_pause(pipeline);
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    /* the stop/finish events of this run must not hit the next playback:
       their handler resets the pipeline and closes the (new) file */
    audio_event_iface_discard(evt);
    pb_mempeak_release();
    pb_progress_reset();

    pb_playing = false;
    pb_default_content = false;
    pb_led_download = false;

    /* close local file handles and free stuff */
    pb_toniefile_close(&pb_toniefile_info);
    /* in case there is a download, also stop that one */
    pb_int_abort_dl();
}

/* after a failed start: audio_pipeline_run() left the pipeline in the error
   state (a second run would only say "already started"); back to init */
static void pb_pipeline_reset(void)
{
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_event_iface_discard(evt);
}

static esp_err_t pb_int_play_file(const char *file, uint64_t nfc_uid)
{
    ESP_LOGI(TAG, "Play: '%s'", file);

    if (pb_toniefile_open(&pb_toniefile_info, file) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to play file: '%s'", file);
        return ESP_FAIL;
    }

    if (!pb_default_content && pb_toniefile_info.taf)
    {
        TonieboxAudioFileHeader *taf = pb_toniefile_info.taf;
        rtnl_audio_id(taf->audio_id);
        rtnl_event(RTNL3_PLAYBACK_STARTING);
        taf_verify_set_playing(pb_toniefile_info.filename);
        /* a complete file: check its hash once per start in the background */
        if (!pb_dl_running(current_dl_req))
        {
            taf_verify_start(pb_toniefile_info.filename, taf->sha1_hash.data, taf->sha1_hash.len, taf->num_bytes,
                             taf->audio_id);
        }
    }

    if (!pb_default_content)
    {
        /* resuming needs block 2 or later: block 1 starts with the Opus header
           again, after the initial header the decoder would see the stream
           twice ("Apparent chaining without changing serial number") */
        uint32_t audio_id = pb_toniefile_info.taf ? pb_toniefile_info.taf->audio_id : 0;
        if (nfc_uid == pb_last_nfc_uid && pb_last_audio_id && audio_id && audio_id != pb_last_audio_id &&
            pb_last_play_position)
        {
            ESP_LOGI(TAG, "UID %16llX: new content (audio ID %08" PRIX32 ", was %08" PRIX32 "), play from start", nfc_uid,
                     audio_id, pb_last_audio_id);
            pb_last_play_position = 0;
        }
        if (nfc_uid == pb_last_nfc_uid && pb_last_play_position + 1 >= 2)
        {
            ESP_LOGI(TAG, "UID %16llX match, set last play position to %d", nfc_uid, pb_last_play_position);
            pb_toniefile_info.current_block = pb_last_play_position + 1;
        }
        else if (nfc_uid == pb_last_nfc_uid)
        {
            ESP_LOGI(TAG, "UID %16llX match, but still at the start, play from start", nfc_uid);
        }
        else
        {
            ESP_LOGI(TAG, "UID %16llX is new, play from start", nfc_uid);
            pb_last_play_position = 0;
            pb_last_nfc_uid = nfc_uid;
        }
        pb_last_audio_id = audio_id;
    }

    /* events of a run that ended on its own (jingle just finished) are stale now */
    audio_event_iface_discard(evt);
    pb_mempeak_taken = mempeak_enter("playback start", 5000);
    pb_mempeak_since = xTaskGetTickCount();
    pb_dec_reserve_give();
    esp_err_t run = audio_pipeline_run(pipeline);
    if (run != ESP_OK)
    {
        /* a task could not be created (no block large enough): tidy up and
           try once more, then give up cleanly instead of looking as if it
           played (silent, no auto power off) */
        ESP_LOGW(TAG, "Pipeline did not start (largest free block %u), trying again",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        pb_pipeline_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
        run = audio_pipeline_run(pipeline);
    }
    if (run != ESP_OK)
    {
        ESP_LOGE(TAG, "Pipeline did not start: not enough memory");
        pb_pipeline_reset();
        pb_mempeak_release();
        pb_progress_reset();
        pb_toniefile_close(&pb_toniefile_info);
        pb_playing = false;
        pb_default_content = false;
        ledman_change("failed");
        return ESP_FAIL;
    }
    audio_pipeline_resume(pipeline);

    return ESP_OK;
}

static esp_err_t pb_int_play_default(uint32_t lang, uint32_t id)
{
    char filename[64];

    pb_startup_jingle = id == CONTENT_DEFAULT_STARTUP;
    pb_startup_since = xTaskGetTickCount();

    sprintf(filename, "/sdcard/CONTENT/%08" PRIX32 "/%08" PRIX32, lang, id);
    if (pb_check_file(filename) == PB_ERR_GOOD_FILE)
    {
        pb_default_content = true;
        return pb_int_play_file(filename, 0);
    }

    if (lang != 0)
    {
        sprintf(filename, "/sdcard/CONTENT/%08" PRIX32 "/%08" PRIX32, (uint32_t)0, id);
        if (pb_check_file(filename) == PB_ERR_GOOD_FILE)
        {
            pb_default_content = true;
            return pb_int_play_file(filename, 0);
        }
    }
    ESP_LOGE(TAG, "requested ID does not exist: '%08X', neither in lang %d nor in default", id, lang);
    return ESP_ERR_NOT_FOUND;
}

/********************************************************/
/* handlers for main loop, only to be called from there */
/********************************************************/

static esp_err_t pb_req_handle_play(pb_req_play_t *req)
{
    pb_int_stop();

    char *filename = pb_build_filename(req->uid);
    /* a complete download that was not renamed yet (power lost) */
    pb_finish_tmp(filename);
    esp_err_t file_state = pb_check_file(filename);

    /* right now we cannot play this file, wait for token to download it */
    if (file_state != PB_ERR_GOOD_FILE)
    {
        free(filename);
        ledman_change("checking");
        return ESP_FAIL;
    }

    pb_int_play_file(filename, req->uid);
    free(filename);

    return ESP_OK;
}

static esp_err_t pb_req_handle_play_token(pb_req_play_token_t *req)
{
    /* already playing? no need to download using token */
    if (pb_playing)
    {
        return ESP_OK;
    }

    char *filename = pb_build_filename(req->uid);
    esp_err_t file_state = pb_check_file(filename);

    /* quite unexpected. should not happen, but play anyway */
    if (file_state == PB_ERR_GOOD_FILE)
    {
        pb_int_play_file(filename, req->uid);
        free(filename);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "requested file shall be downloaded first: '%s'", filename);

    /* if there is already a download running, cancel it first */
    pb_int_abort_dl();

    /* right after power on (tonie already on the box) or a WiFi drop: wait
       for WiFi and teddyCloud instead of failing at once with the network
       error sound */
    if (cloud_is_enabled() && !cloud_is_ready())
    {
        ESP_LOGI(TAG, "Waiting for WiFi and teddyCloud before the download...");
        ledman_change("checking");
        int64_t until = esp_timer_get_time() + PB_CLOUD_WAIT_MS * 1000LL;
        while (!cloud_is_ready() && esp_timer_get_time() < until)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            taskwatch_feed(pb_taskwatch);
            if (nfc_get_current_uid() != req->uid)
            {
                ESP_LOGI(TAG, "Tonie taken off while waiting for the network");
                free(filename);
                ledman_change("idle");
                return ESP_OK;
            }
        }
        ESP_LOGI(TAG, "Network %s", cloud_is_ready() ? "ready" : "still not ready, trying anyway");
    }

    ESP_LOGI(TAG, "initiate download");
    current_dl_req = cloud_content_download(req->uid, req->token);

    if (!pb_default_content && pb_last_nfc_uid != req->uid)
    {
        pb_last_play_position = 0;
    }
    else if (!pb_default_content && pb_last_play_position)
    {
        /* the position counts only for the same content: a download that
           continues a .tmp of it (tonie taken off while downloading) keeps
           it, a new download (new episode, or the file was gone) starts from
           the beginning instead of waiting for blocks far into the file */
        uint32_t tmp_id = pb_file_audio_id(current_dl_req ? current_dl_req->tmpname : NULL);
        if (!tmp_id || (pb_last_audio_id && tmp_id != pb_last_audio_id))
        {
            ESP_LOGI(TAG, "New download (audio ID %08" PRIX32 ", played %08" PRIX32 "): play from start", tmp_id,
                     pb_last_audio_id);
            pb_last_play_position = 0;
        }
    }

    bool proceed = false;
    bool waiting = true;
    uint32_t start_pos = (pb_default_content ? 0 : pb_last_play_position) + PB_MIN_DL_BLOCKS;
    /* cloud unreachable or no time sync yet: do not block the playback thread forever */
    int connect_wait = PB_DL_CONNECT_TIMEOUT_MS / 100;

    while (waiting)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        /* a slow download that is still coming is no hang */
        taskwatch_feed(pb_taskwatch);
        switch (cloud_content_get_state(current_dl_req))
        {
        case CC_STATE_INIT:
        case CC_STATE_CONNECTING:
        case CC_STATE_RESUMING:
            if (--connect_wait <= 0)
            {
                ESP_LOGE(TAG, "Download did not start in time");
                pb_int_abort_dl();
                waiting = false;
                proceed = false;
            }
            break;

        case CC_STATE_ABORTED:
            break;

        case CC_STATE_CONNECTED:
            ESP_LOGI(TAG, "Connected...");
            break;

        case CC_STATE_ERROR:
            ESP_LOGE(TAG, "Download failed");
            waiting = false;
            proceed = false;
            /* closes the file handle and frees the request */
            pb_int_abort_dl();
            break;

        case CC_STATE_FINISHED:
            ESP_LOGI(TAG, "Download finished");
            waiting = false;
            proceed = true;
            /* complete on disk, playback opens its own handle */
            pb_int_abort_dl();
            break;

        case CC_STATE_RECEIVING:
            if (current_dl_req->is_stream && start_pos != PB_MIN_DL_BLOCKS)
            {
                /* a stream starts anew every time: no position to resume */
                ESP_LOGI(TAG, "Stream: playing from its start");
                pb_last_play_position = 0;
                start_pos = PB_MIN_DL_BLOCKS;
            }
            if (current_dl_req->received / 4096 > start_pos)
            {
                ESP_LOGI(TAG, "Download in progress, enough blocks received");
                waiting = false;
                proceed = true;
            }
            break;
        }
    }

    if (!proceed)
    {
        ESP_LOGE(TAG, "...could not download the file");

        /* the freshness check parks outdated content as .old: better the old version than nothing */
        char old_name[64];
        struct stat st;
        snprintf(old_name, sizeof(old_name), "%s.old", filename);
        if (stat(old_name, &st) == 0)
        {
            ESP_LOGW(TAG, "Playing previous version of '%s' instead", filename);
            unlink(filename);
            if (rename(old_name, filename) == 0)
            {
                esp_err_t ret = pb_int_play_file(filename, req->uid);
                free(filename);
                return ret;
            }
        }

        free(filename);
        ledman_change("failed");
        /* audible: the original's sound for connection problems */
        pb_int_stop();
        pb_int_play_default(settings_get()->lang_id, CONTENT_DEFAULT_NETWORK_ERROR);
        return ESP_ERR_NOT_FOUND;
    }

    return pb_int_play_file(filename, req->uid);
}

static esp_err_t pb_req_handle_stop(pb_req_stop_t *req)
{
    pb_int_stop();
    return ESP_OK;
}

static esp_err_t pb_req_handle_default(pb_req_default_t *req)
{
    pb_int_stop();
    return pb_int_play_default(settings_get()->lang_id, req->voiceline);
}

/********************************************************/
/* main loop, calls functions above                     */
/********************************************************/

static void pb_stream_ended(uint32_t received, int64_t end_us)
{
    uint64_t uid = pb_get_current_uid();
    int64_t gap_ms = end_us ? (esp_timer_get_time() - end_us) / 1000 : 0;
    if (gap_ms >= PB_STREAM_FINITE_GAP_MS)
    {
        ESP_LOGI(TAG, "Stream played to the end (%u bytes, transfer ended %lld s before): not requested again",
                 (unsigned)received, (long long)(gap_ms / 1000));
        pb_stream_short = 0;
        return;
    }
    if (uid != pb_stream_short_uid)
    {
        pb_stream_short_uid = uid;
        pb_stream_short = 0;
    }
    pb_stream_short = received < PB_STREAM_SHORT_BYTES ? pb_stream_short + 1 : 0;
    if (pb_stream_short >= PB_STREAM_SHORT_MAX)
    {
        ESP_LOGE(TAG, "Stream: ended %d times right after the start, giving up", pb_stream_short);
        pb_stream_short = 0;
        pb_play_default(CONTENT_DEFAULT_NETWORK_ERROR);
        return;
    }
    if (uid && nfc_get_current_uid() == uid)
    {
        ESP_LOGI(TAG, "Stream ended after %u bytes, tonie still there: again in %d ms", (unsigned)received,
                 PB_STREAM_RESTART_DELAY_MS);
        pb_stream_restart_uid = uid;
        pb_stream_restart_at = xTaskGetTickCount() + pdMS_TO_TICKS(PB_STREAM_RESTART_DELAY_MS);
    }
}

void pb_mainthread(void *arg)
{
    pb_playing = false;
    pb_headset = dac3100_headset_detected() != 0;
    dac3100_set_output(pb_headset ? DAC3100_OUT_HEADPHONE : DAC3100_OUT_SPEAKER);
    ESP_LOGI(TAG, "Listen for all pipeline events");
    pb_taskwatch = taskwatch_add("playback", 90000);

    while (1)
    {
        vTaskDelay(50 / portTICK_PERIOD_MS);
        taskwatch_feed(pb_taskwatch);

        /* current_dl_req belongs to this thread, so it is safe to look at here */
        {
            cloud_content_req_t *dl = current_dl_req;
            pb_prog_dl = (dl && dl->content_length) ? pb_percent(dl->received, dl->content_length) : -1;
            /* the download ended (complete or not) while playing: back to the normal colour */
            if (pb_led_download && !pb_dl_running(dl))
            {
                pb_led_download = false;
                ledman_change("playing");
            }
        }

        pb_audio_rate_check();
        pb_decoder_load_log();

        /* keep the block for the next decoder task (see pb_dec_reserve):
           a moment after the end, the old task's stack is freed by then */
        if (pb_playing)
        {
            pb_idle_since = xTaskGetTickCount();
        }
        else if (!pb_dec_reserve && xTaskGetTickCount() - pb_idle_since > pdMS_TO_TICKS(1000))
        {
            pb_dec_reserve_take();
        }

        /* the decoder normally reports its format within a few 100 ms */
        if (pb_mempeak_taken && xTaskGetTickCount() - pb_mempeak_since > pdMS_TO_TICKS(PB_MEMPEAK_MAX_MS))
        {
            ESP_LOGW(TAG, "No music info from the decoder yet");
            pb_mempeak_release();
        }

        if (board_headset_irq())
        {
            uint8_t type = dac3100_headset_detected();
            pb_headset = type != 0;
            ESP_LOGI(TAG, "Headset detected: %s", type ? "YES" : "NO");
            dac3100_set_output(pb_headset ? DAC3100_OUT_HEADPHONE : DAC3100_OUT_SPEAKER);
            dac3100_set_mute(!pb_is_playing());
        }

        if (pb_stream_restart_uid && (int32_t)(xTaskGetTickCount() - pb_stream_restart_at) >= 0)
        {
            uint64_t uid = pb_stream_restart_uid;
            pb_stream_restart_uid = 0;
            if (!pb_playing && nfc_get_current_uid() == uid)
            {
                ESP_LOGI(TAG, "Stream: requesting it again");
                pb_play_content_token(uid, nfc_get_current_token());
            }
        }

        pb_req_t *req = NULL;
        /* the start jingle plays to the end (as the original); a tonie
           placed meanwhile, or already on the box at power on, waits for it.
           That also gives WiFi a few seconds. */
        bool hold = false;
        if (pb_startup_jingle)
        {
            TickType_t age = xTaskGetTickCount() - pb_startup_since;
            /* pb_playing comes with the "Run" event a moment after the start */
            if ((!pb_playing && age >= pdMS_TO_TICKS(1000)) || !pb_default_content ||
                age >= pdMS_TO_TICKS(PB_STARTUP_JINGLE_MAX_MS))
            {
                pb_startup_jingle = false;
            }
            else if (xQueuePeek(playback_queue, &req, 0) == pdTRUE && req->type != PB_REQ_TYPE_DEFAULT)
            {
                hold = true;
            }
        }
        if (!hold && xQueueReceive(playback_queue, &req, 0) == pdTRUE)
        {
            switch (req->type)
            {
            case PB_REQ_TYPE_PLAY:
                pb_req_handle_play((pb_req_play_t *)req);
                break;
            case PB_REQ_TYPE_PLAY_TOKEN:
                pb_req_handle_play_token((pb_req_play_token_t *)req);
                break;
            case PB_REQ_TYPE_STOP:
                pb_req_handle_stop((pb_req_stop_t *)req);
                break;
            case PB_REQ_TYPE_DEFAULT:
                pb_req_handle_default((pb_req_default_t *)req);
                break;
            default:
                break;
            }
            free(req);
        }

        audio_event_iface_msg_t msg;
        if (audio_event_iface_listen(evt, &msg, 0) == ESP_OK)
        {
            switch (msg.source_type)
            {
            default:
                ESP_LOGI(TAG, "[ * ] Receive info from %d", msg.source_type);
                break;
            case PERIPH_ID_SDCARD:
            {
                switch (msg.cmd)
                {
                case SDCARD_STATUS_UNKNOWN:
                    ESP_LOGI(TAG, "SDCARD_STATUS_UNKNOWN");
                    break;
                case SDCARD_STATUS_CARD_DETECT_CHANGE:
                    ESP_LOGI(TAG, "SDCARD_STATUS_CARD_DETECT_CHANGE");
                    break;
                case SDCARD_STATUS_MOUNTED:
                    ESP_LOGI(TAG, "SDCARD_STATUS_MOUNTED");
                    break;
                case SDCARD_STATUS_UNMOUNTED:
                    ESP_LOGI(TAG, "SDCARD_STATUS_UNMOUNTED");
                    break;
                case SDCARD_STATUS_MOUNT_ERROR:
                    ESP_LOGI(TAG, "SDCARD_STATUS_MOUNT_ERROR");
                    break;
                case SDCARD_STATUS_UNMOUNT_ERROR:
                    ESP_LOGI(TAG, "SDCARD_STATUS_UNMOUNT_ERROR");
                    break;
                }
                break;
            }
            case AUDIO_ELEMENT_TYPE_ELEMENT:
            {
                if (msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
                {
                    // msg.source == (void *)music_decoder &&
                    audio_element_info_t music_info = {0};
                    if (audio_element_getinfo(music_decoder, &music_info) == ESP_OK)
                    {
                        ESP_LOGI(TAG, "[ * ] Receive music info from decoder, sample_rates=%d, bits=%d, ch=%d",
                                 music_info.sample_rates, music_info.bits, music_info.channels);
                        /* decoder buffers are allocated now */
                        pb_mempeak_release();
                        memdiag_log("playback running");
                        audio_element_setinfo(i2s_stream_writer, &music_info);
                        if (i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels) != ESP_OK)
                        {
                            ESP_LOGE(TAG, "Failed to set I²S rate");
                        }
                    }
                }
                else if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
                {
                    const char *source = "???";
                    if (msg.source == (void *)i2s_stream_writer)
                    {
                        source = "I²S";
                    }
                    if (msg.source == (void *)music_decoder)
                    {
                        source = "OPUS";
                    }

                    switch ((int)msg.data)
                    {
                    case AEL_STATUS_STATE_PAUSED:
                        ESP_LOGW(TAG, "[Event] [%s] Pause", source);
                        if (msg.source == (void *)i2s_stream_writer)
                        {
                            pb_i2s_paused = true;
                        }
                        pb_playing = true;
                        dac3100_set_mute(true);
                        pb_led_download = false;
                        if (!pb_default_content)
                        {
                            ledman_change("idle");
                        }
                        break;
                    case AEL_STATUS_STATE_RUNNING:
                        ESP_LOGW(TAG, "[Event] [%s] Run", source);
                        if (msg.source == (void *)i2s_stream_writer)
                        {
                            pb_i2s_paused = false;
                            if (!pb_default_content)
                            {
                                rtnl_event(RTNL3_PLAYBACK_STARTED);
                            }
                        }
                        pb_playing = true;
                        dac3100_set_mute(false);
                        if (!pb_default_content)
                        {
                            pb_led_download = pb_dl_running(current_dl_req);
                            ledman_change(pb_led_download ? "playing download" : "playing");
                        }
                        break;
                    case AEL_STATUS_STATE_STOPPED:
                    case AEL_STATUS_STATE_FINISHED:
                        ESP_LOGW(TAG, "[Event] [%s] Stop", source);
                        pb_mempeak_release();
                        if (msg.source == (void *)i2s_stream_writer)
                        {
                            if (!pb_default_content)
                            {
                                rtnl_event(RTNL3_PLAYBACK_STOPPED);
                            }
                            dac3100_set_mute(true);
                            pb_led_download = false;
                            if (!pb_default_content)
                            {
                                ledman_change("idle");
                            }
                            cloud_content_req_t *dl = current_dl_req;
                            if (!pb_default_content && dl && dl->is_stream && !dl->abort && !pb_dl_running(dl) &&
                                (int)msg.data == AEL_STATUS_STATE_FINISHED)
                            {
                                pb_stream_ended(dl->received, dl->end_us);
                            }
                            pb_default_content = false;
                            pb_playing = false;
                            pb_progress_reset();
                            audio_pipeline_reset_ringbuffer(pipeline);
                            audio_pipeline_reset_elements(pipeline);
                            audio_pipeline_change_state(pipeline, AEL_STATE_INIT);

                            /* close handle and free stuff, if not done before */
                            pb_toniefile_close(&pb_toniefile_info);
                            /* stop any pending download and close file handles */
                            pb_int_abort_dl();
                        }
                        break;
                    default:
                        ESP_LOGW(TAG, "[Event] [%s] %d", source, (int)msg.data);
                        if ((int)msg.data == AEL_STATUS_ERROR_PROCESS || (int)msg.data == AEL_STATUS_ERROR_OPEN)
                        {
                            pb_mempeak_release();
                        }
                        break;
                    }
                }
                break;
            }
            }
        }
    }
}

void pb_init(esp_periph_set_handle_t set)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    playback_queue = xQueueCreate(PB_QUEUE_SIZE, sizeof(char *));
    ESP_LOGI(TAG, "Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    pb_mon_start();
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    /* same DMA buffering as with IDF 4.4: 4 x 512 frames */
    i2s_cfg.chan_cfg.dma_desc_num = 4;
    i2s_cfg.chan_cfg.dma_frame_num = 512;

    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    opus_decoder_cfg_t opus_dec_cfg = DEFAULT_OPUS_DECODER_CONFIG();
    opus_dec_cfg.stack_in_ext = false;
    /* was 200, which FreeRTOS clamped to the highest priority; IDF 5 asserts instead */
    opus_dec_cfg.task_prio = configMAX_PRIORITIES - 1;
    /* PCM between decoder and I2S: 4096 bytes were only 21 ms at 48 kHz stereo,
       an SD access that took longer while a download wrote was audible */
    opus_dec_cfg.out_rb_size = 8 * 1024;
    /* core 0 has WiFi, lwIP, esp_timer and the I2S output: the decoder on the
       mostly idle core 1 does not delay the output */
    opus_dec_cfg.task_core = 1;
    if (settings_get()->decoder_celt)
    {
        music_decoder = tb_opus_dec_init(opus_dec_cfg.task_core, opus_dec_cfg.task_prio);
    }
    else
    {
        music_decoder = decoder_opus_init(&opus_dec_cfg);
    }

    audio_pipeline_register(pipeline, music_decoder, "dec");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    const char *link_tag[2] = {"dec", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);
    audio_element_set_read_cb(music_decoder, &pb_toniefile_cbr, &pb_toniefile_info);

    ESP_LOGI(TAG, "Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt = audio_event_iface_init(&evt_cfg);

    audio_pipeline_set_listener(pipeline, evt);
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    xTaskCreatePinnedToCore(pb_mainthread, "[TB] Playback", PB_TASK_STACK, NULL, PB_TASK_PRIO, NULL, tskNO_AFFINITY);
}

void pb_deinit()
{
    ESP_LOGI(TAG, "Stop audio_pipeline");

    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, music_decoder);

    audio_event_iface_destroy(evt);

    audio_pipeline_remove_listener(pipeline);

    audio_pipeline_deinit(pipeline);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(music_decoder);
}