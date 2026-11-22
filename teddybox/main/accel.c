

#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lis3dh.h"
#include "lis3dh_regs.h"
#include "settings.h"

#include "accel.h"
#include "board.h"
#include "playback.h"
#include "rtnl.h"

static const char *TAG = "[ACC]";

static float getRoll(float ax, float ay, float az)
{
    return atan2(ay, az) * 180.0 / M_PI;
}

static float getPitch(float ax, float ay, float az)
{
    return atan2(-ax, sqrt(ay * ay + az * az)) * 180.0 / M_PI;
}

typedef enum
{
    STATE_UNKNOWN,
    STATE_STABLE_MAYBE,
    STATE_STABLE,
    STATE_TILTED_MAYBE,
    STATE_TILTED,
    STATE_FLIPPED_MAYBE,
    STATE_FLIPPED
} accel_state_t;

static accel_state_t current_state = STATE_UNKNOWN;
static volatile bool upside_down = false;
static volatile int64_t last_sample_us = 0;

bool accel_upside_down(void) { return accel_working() && upside_down; }

bool accel_working(void) { return last_sample_us && esp_timer_get_time() - last_sample_us < 1000000; }
uint32_t state_counter = 0;

bool accel_within(float val, float target, float limit)
{
    float difference = fabs(fabs(val) - target);

    if (difference < limit)
    {
        return true;
    }

    if (difference > 360.0 - limit)
    {
        return true;
    }

    return false;
}

void accel_handle_angle(float roll, float pitch, float tilt)
{
    static float target_roll = 0;
    static float target_pitch = 0;
    static float target_limit = 0;
    static bool was_stable = false;

    if (!accel_within(roll, target_roll, target_limit) || !accel_within(pitch, target_pitch, target_limit))
    {
        current_state = STATE_UNKNOWN;
        state_counter = 0;
    }

    switch (current_state)
    {
    case STATE_UNKNOWN:
    {
        if (accel_within(roll, ACCEL_ANGLE_STABLE, tilt / 2) && accel_within(pitch, ACCEL_ANGLE_STABLE, tilt / 2))
        {
            target_roll = 0;
            target_pitch = 0;
            target_limit = tilt / 2;
            current_state = STATE_STABLE_MAYBE;
            ESP_LOGI(TAG, "STATE_STABLE_MAYBE");
        }
        if (accel_within(roll, tilt, tilt / 2) && accel_within(pitch, tilt, tilt / 2))
        {
            target_roll = tilt;
            target_pitch = tilt;
            target_limit = tilt / 2;
            current_state = STATE_TILTED_MAYBE;
            ESP_LOGI(TAG, "STATE_TILTED_MAYBE");
        }
        if (accel_within(roll, ACCEL_ANGLE_FLIP, tilt / 2) && accel_within(pitch, ACCEL_ANGLE_STABLE, tilt / 2))
        {
            target_roll = ACCEL_ANGLE_FLIP;
            target_pitch = ACCEL_ANGLE_STABLE;
            target_limit = tilt / 2;
            current_state = STATE_FLIPPED_MAYBE;
            ESP_LOGI(TAG, "STATE_FLIPPED_MAYBE");
        }
        break;
    }

    case STATE_STABLE_MAYBE:
    case STATE_TILTED_MAYBE:
    case STATE_FLIPPED_MAYBE:
        if (state_counter++ >= ACCEL_STABLE_DELAY)
        {
            ESP_LOGI(TAG, "Considering angle stable");
            current_state++;
            state_counter = 0;
        }
        break;

    case STATE_STABLE:
        was_stable = true;
        break;

    case STATE_TILTED:
        if (roll > 0 && pitch > 0)
        {
            if (state_counter++ == 0 && was_stable)
            {
                was_stable = false;
                ESP_LOGI(TAG, "emit CHAPTER -");
                rtnl_event(RTNL3_TILT_BACKWARD);
                pb_seek_chapter(-1);
            }
        }
        if (roll < 0 && pitch < 0)
        {
            if (state_counter++ == 0 && was_stable)
            {
                was_stable = false;
                ESP_LOGI(TAG, "emit CHAPTER +");
                rtnl_event(RTNL3_TILT_FORWARD);
                pb_seek_chapter(1);
            }
        }
        if (roll < 0 && pitch > 0)
        {
            if (state_counter++ == 0)
            {
                was_stable = false;
                ESP_LOGI(TAG, "emit SEEK +");
                pb_seek(ACCEL_SEEK_BLOCKS);
            }
            /* repeat action */
            if (state_counter > ACCEL_SEEK_REPEAT)
            {
                state_counter = 0;
            }
        }
        if (roll > 0 && pitch < 0)
        {
            if (state_counter++ == 0)
            {
                was_stable = false;
                ESP_LOGI(TAG, "emit SEEK -");
                pb_seek(-ACCEL_SEEK_BLOCKS);
            }
            /* repeat action */
            if (state_counter > ACCEL_SEEK_REPEAT)
            {
                state_counter = 0;
            }
        }
        break;

    case STATE_FLIPPED:
        ESP_LOGI(TAG, "emit FLIPPED");
        break;
    }
}

/* sensitivity levels 0 (off) .. 5, 3 = default */
static const uint8_t slap_threshold[ACCEL_LEVELS] = {0, 127, 113, 100, 85, 70}; /* x 62.5 mg, 100 = original */
static const float tilt_angle[ACCEL_LEVELS] = {0, 40, 35, 30, 25, 20};         /* degrees, 30 = before */

static SemaphoreHandle_t click_sem;
static volatile int64_t click_us;

static void IRAM_ATTR click_isr(void *arg)
{
    click_us = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(click_sem, &woken);
    if (woken)
    {
        portYIELD_FROM_ISR();
    }
}

/* signed span of one axis over the FIFO: max - min, negative if the
   negative swing is the bigger one (like the original firmware) */
static int axis_span(const int16_t *s, int n, int axis)
{
    int mn = INT16_MAX, mx = INT16_MIN;
    for (int i = 0; i < n; i++)
    {
        int v = s[i * 3 + axis];
        mn = v < mn ? v : mn;
        mx = v > mx ? v : mx;
    }
    int span = mx - mn;
    return abs(mx) < abs(mn) ? -span : span;
}

/* Evaluation of the original firmware: after the click interrupt wait until
   40 ms have passed, take the 32 FIFO samples (80 ms) and compare the swing
   of the axes. A slap on the side moves Y and Z (the sensor sits at an
   angle) more than X; Y and Z both positive is one side, both negative the
   other, anything else is no slap. Returns +1, -1 or 0. */
static int accel_slap_direction(lis3dh_t lis)
{
    int64_t since = esp_timer_get_time() - click_us;
    if (since < 40000)
    {
        vTaskDelay(pdMS_TO_TICKS((40000 - since) / 1000 + 1));
    }
    uint8_t fifo_src = 0, click_src = 0;
    lis3dh_get_reg(lis, LIS3DH_FIFO_SRC_REG, &fifo_src);
    int n = (fifo_src & 0x40) ? 32 : (fifo_src & 0x1F); /* OVRN: full */
    static int16_t samples[32 * 3];
    bool fifo_ok = n == 32 && lis3dh_read_burst(lis, 0xA8, (uint8_t *)samples, sizeof(samples)) == ESP_OK;
    /* reading CLICK_SRC releases the latched interrupt */
    lis3dh_get_reg(lis, LIS3DH_CLICK_SRC, &click_src);
    if (!fifo_ok || !(click_src & 0x40))
    {
        ESP_LOGI(TAG, "Click without data (FIFO %d, CLICK_SRC %02X)", n, click_src);
        return 0;
    }
    int dx = axis_span(samples, 32, 0), dy = axis_span(samples, 32, 1), dz = axis_span(samples, 32, 2);
    int dir = 0;
    if (abs(dx) < (abs(dy) < abs(dz) ? abs(dy) : abs(dz)))
    {
        dir = (dy > 0 && dz > 0) ? 1 : (dy < 0 && dz < 0) ? -1 : 0;
    }
    /* raw values, 1 g = 4096 at +-8 g */
    ESP_LOGI(TAG, "Slap: swing X %d Y %d Z %d -> %s", dx, dy, dz, dir > 0 ? "side A" : dir < 0 ? "side B" : "no slap");
    return dir;
}

void accel_mainthread(void *arg)
{
    audio_board_handle_t board = (audio_board_handle_t)arg;
    lis3dh_t lis = board->lis3dh;
    float accel[3];
    int slap_level = -1;
    int64_t last_slap_us = 0;

    click_sem = xSemaphoreCreateBinary();
    gpio_set_intr_type(LIS3DH_IRQ_GPIO, GPIO_INTR_POSEDGE);
    gpio_isr_handler_add(LIS3DH_IRQ_GPIO, click_isr, NULL);

    while (1)
    {
        /* settings can change at any time (Home Assistant, setup page) */
        const settings_t *s = settings_get();
        int level = s->slap_level < ACCEL_LEVELS ? s->slap_level : 3;
        if (level != slap_level)
        {
            slap_level = level;
            if (lis3dh_setup_click(lis, slap_threshold[level]) != ESP_OK)
            {
                ESP_LOGW(TAG, "Sensor setup failed");
            }
            ESP_LOGI(TAG, "Slap %s (threshold %d), tilt level %d", level ? "on" : "off", slap_threshold[level], s->tilt_level);
            uint8_t dummy;
            lis3dh_get_reg(lis, LIS3DH_CLICK_SRC, &dummy);
            xSemaphoreTake(click_sem, 0);
        }

        /* wait for a click or the next tilt sample */
        if (xSemaphoreTake(click_sem, pdMS_TO_TICKS(ACCEL_LOOP_MS)) == pdTRUE)
        {
            int dir = slap_level ? accel_slap_direction(lis) : 0;
            /* the box swings after a slap: one chapter per slap */
            if (dir && esp_timer_get_time() - last_slap_us > ACCEL_SLAP_PAUSE_MS * 1000LL)
            {
                last_slap_us = esp_timer_get_time();
                if (upside_down)
                {
                    /* upside down, either side: start over (as Hackiebox CFW) */
                    ESP_LOGI(TAG, "emit START OVER (slap upside down)");
                    rtnl_event(RTNL3_KNOCK_BACKWARD);
                    pb_set_chapter(0);
                    continue;
                }
                if (s->slap_reverse)
                {
                    dir = -dir;
                }
                ESP_LOGI(TAG, "emit CHAPTER %s (slap)", dir > 0 ? "+" : "-");
                rtnl_event(dir > 0 ? RTNL3_KNOCK_FORWARD : RTNL3_KNOCK_BACKWARD);
                pb_seek_chapter(dir);
            }
            continue;
        }

        esp_err_t err = lis->fetch(lis, accel);
        if (err != ESP_OK)
        {
            continue;
        }
        float x = accel[2];
        float y = accel[1];
        float z = -accel[0];

        float roll = getRoll(x, y, z);
        float pitch = getPitch(x, y, z);
        upside_down = accel_within(roll, ACCEL_ANGLE_FLIP, ACCEL_UPSIDE_DOWN_LIMIT) &&
                      accel_within(pitch, ACCEL_ANGLE_STABLE, ACCEL_UPSIDE_DOWN_LIMIT);
        last_sample_us = esp_timer_get_time();

        int tilt = s->tilt_level < ACCEL_LEVELS ? s->tilt_level : 3;
        if (tilt)
        {
            accel_handle_angle(roll, pitch, tilt_angle[tilt]);
        }
    }
}

void accel_init(audio_board_handle_t board)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    /* 2048 left only 200 bytes on the box after the first tilt (log output) */
    xTaskCreatePinnedToCore(accel_mainthread, "[TB] accel", 3072, (void *)board, ACCEL_TASK_PRIO, NULL, tskNO_AFFINITY);
}
