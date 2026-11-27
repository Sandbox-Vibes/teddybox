/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2022 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "esp_log.h"
#include "board.h"
#include "audio_mem.h"
#include "esp_lcd_panel_ops.h"
#include "periph_sdcard.h"
#include "periph_lcd.h"
#include "periph_adc_button.h"
#include "esp_sleep.h"

#include "driver/rtc_io.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "soc/rtc.h"
#include "esp_sleep.h"

#include "dac3100.h"
#include "driver/gpio.h"
#include "lis3dh.h"
#include "trf7962a.h"
#include "led.h"

static const char *TAG = "TB-ESP32";

static audio_board_handle_t board_handle = 0;
static esp_periph_handle_t sdcard_handle = 0;
static bool i2c_lis3dh_ok = false;
static bool i2c_dac_ok = false;

static esp_err_t i2c_init(audio_board_handle_t board)
{
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000};
    int res = get_i2c_pins(I2C_NUM_0, &i2c_cfg);
    board->i2c_handle = i2c_bus_create(I2C_NUM_0, &i2c_cfg);

    if (!board->i2c_handle)
    {
        ESP_LOGE(TAG, "I2C init failed");
        return ESP_FAIL;
    }

    uint8_t reg_add = 0;
    uint8_t reg_data = 0;
    esp_err_t ret;

    ESP_LOGE(TAG, "I2C checks:");
    reg_add = 0x0F;
    ret = i2c_bus_read_bytes(board->i2c_handle, 0x32, &reg_add, 1, &reg_data, 1);
    i2c_lis3dh_ok = (ret == ESP_OK) && (reg_data == 0x33);
    ESP_LOGE(TAG, "  LIS3DH:  %s", i2c_lis3dh_ok ? "[OK]" : "[FAIL]");

    reg_add = 0x00;
    ret = i2c_bus_read_bytes(board->i2c_handle, DAC3100_ADDR, &reg_add, 1, &reg_data, 1);
    i2c_dac_ok = ret == ESP_OK;
    ESP_LOGE(TAG, "  DAC3100: %s", i2c_dac_ok ? "[OK]" : "[FAIL]");

    return res;
}

static esp_err_t spi_init(audio_board_handle_t board)
{
    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .miso_io_num = SPI_MISO_GPIO,
        .mosi_io_num = SPI_MOSI_GPIO,
        .sclk_io_num = SPI_SCLK_GPIO,
        .max_transfer_sz = 64};

    board->spi_dev = SPI2_HOST;

    ret = spi_bus_initialize(board->spi_dev, &buscfg, SPI_DMA_CH_AUTO);

    return ret;
}

static QueueHandle_t gpio_evt_queue = NULL;

void headset_isr(void *ctx)
{
    uint32_t value = 0;
    xQueueSendFromISR(gpio_evt_queue, &value, NULL);
}

bool board_headset_irq()
{
    uint32_t io_num;
    if (xQueueReceive(gpio_evt_queue, &io_num, 0))
    {
        return true;
    }
    return false;
}

audio_board_handle_t audio_board_init(void)
{
    if (board_handle)
    {
        ESP_LOGW(TAG, "The board has already been initialized!");
        return board_handle;
    }
    board_handle = (audio_board_handle_t)audio_calloc(1, sizeof(struct audio_board_handle));
    AUDIO_MEM_CHECK(TAG, board_handle, return NULL);

    ESP_LOGI(TAG, "Initializing GPIO");
    gpio_config_t io_conf = {0};
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    /* the power switches were held in their off state during deep sleep,
       release them first or they cannot be switched on again */
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(POWER_GPIO);
    gpio_hold_dis(SD_POWER_GPIO);

    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask =
        BIT64(LED_BLUE_GPIO) | BIT64(LED_GREEN_GPIO) | BIT64(LED_RED_GPIO) |
        BIT64(POWER_GPIO) | BIT64(SD_POWER_GPIO) | BIT64(DAC3100_RESET_GPIO) |
        BIT64(SPI_SS_GPIO);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask =
        BIT64(GPIO_NUM_0) | BIT64(EAR_BIG_GPIO) | BIT64(EAR_SMALL_GPIO) |
        BIT64(HEADPHONE_DETECT) | BIT64(TRF7962A_IRQ_GPIO) | BIT64(LIS3DH_IRQ_GPIO) |
        BIT64(WAKEUP_GPIO);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    /* set LED to default states */
    led_init();
    led_set_rgb(0, 0, 0);

    /* init peripherals */
    ESP_LOGI(TAG, "Initializing Peripherals");
    audio_board_power(true);

    gpio_set_level(DAC3100_RESET_GPIO, 0);
    vTaskDelay(10 / portTICK_RATE_MS);
    gpio_set_level(DAC3100_RESET_GPIO, 1);
    vTaskDelay(10 / portTICK_RATE_MS);

    ESP_LOGI(TAG, " Initializing SPI");
    spi_init(board_handle);

    ESP_LOGI(TAG, "  - TRF7962A");
    board_handle->trf7962a = trf7962a_init(board_handle->spi_dev, SPI_SS_GPIO);

    ESP_LOGI(TAG, " Initializing I²C");
    i2c_init(board_handle);

    ESP_LOGI(TAG, "  - LS3DH");
    board_handle->lis3dh = lis3dh_init(board_handle->i2c_handle);

    ESP_LOGI(TAG, "  - DAC3100");
    board_handle->audio_hal = audio_board_codec_init();

    /* setup ISRs for INT lines */
    gpio_isr_handler_add(HEADPHONE_DETECT, headset_isr, NULL);
    gpio_set_intr_type(HEADPHONE_DETECT, GPIO_INTR_POSEDGE);
    gpio_intr_enable(HEADPHONE_DETECT);

    gpio_isr_handler_add(TRF7962A_IRQ_GPIO, trf7962a_isr, board_handle->trf7962a);
    gpio_set_intr_type(TRF7962A_IRQ_GPIO, GPIO_INTR_POSEDGE);
    gpio_intr_enable(TRF7962A_IRQ_GPIO);


    return board_handle;
}

audio_hal_handle_t audio_board_codec_init(void)
{
    audio_hal_codec_config_t audio_codec_cfg = AUDIO_CODEC_DEFAULT_CONFIG();
    audio_hal_handle_t codec_hal = audio_hal_init(&audio_codec_cfg, &AUDIO_CODEC_DAC3100_DEFAULT_HANDLE);
    AUDIO_NULL_CHECK(TAG, codec_hal, return NULL);
    return codec_hal;
}

esp_err_t audio_board_sdcard_unmount()
{
    if(esp_periph_stop(sdcard_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Stopping SD failed");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

bool audio_board_selftest(char *failed, size_t size)
{
    failed[0] = 0;
    bool sd = sdcard_handle && periph_sdcard_is_mounted(sdcard_handle);
    bool nfc = board_handle && board_handle->trf7962a && board_handle->trf7962a->valid;
    bool accel = board_handle && board_handle->lis3dh && board_handle->lis3dh->valid;
    snprintf(failed, size, "%s%s%s%s", sd ? "" : "SD card ", i2c_dac_ok ? "" : "DAC3100 ",
             (i2c_lis3dh_ok && accel) ? "" : "LIS3DH ", nfc ? "" : "TRF7962A");
    return sd && i2c_dac_ok && i2c_lis3dh_ok && accel && nfc;
}

esp_err_t audio_board_sdcard_init(esp_periph_set_handle_t set, periph_sdcard_mode_t mode)
{
    if (mode != SD_MODE_4_LINE)
    {
        ESP_LOGE(TAG, "Current board only support 4-line SD mode!");
        return ESP_FAIL;
    }
    periph_sdcard_cfg_t sdcard_cfg = {
        .root = "/sdcard",
        .card_detect_pin = -1,
        .mode = mode};
    sdcard_handle = periph_sdcard_init(&sdcard_cfg);
    esp_err_t ret = esp_periph_start(set, sdcard_handle);
    int retry_time = 5;
    bool mount_flag = false;
    while (retry_time--)
    {
        if (periph_sdcard_is_mounted(sdcard_handle))
        {
            mount_flag = true;
            break;
        }
        else
        {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
    if (mount_flag == false)
    {
        ESP_LOGE(TAG, "SD card mount failed");
        esp_periph_stop(sdcard_handle);
        return ESP_FAIL;
    }
    return ret;
}

audio_board_handle_t audio_board_get_handle(void)
{
    return board_handle;
}

audio_hal_handle_t audio_board_get_hal(void)
{
    return board_handle->audio_hal;
}

trf7962a_t audio_board_get_trf(void)
{
    return board_handle->trf7962a;
}

esp_err_t audio_board_deinit(audio_board_handle_t audio_board)
{
    esp_err_t ret = ESP_OK;
    ret |= audio_hal_deinit(audio_board->audio_hal);
    audio_free(audio_board);
    board_handle = NULL;
    return ret;
}

bool audio_board_ear_big()
{
    return gpio_get_level(EAR_BIG_GPIO) == 0;
}

bool audio_board_ear_small()
{
    return gpio_get_level(EAR_SMALL_GPIO) == 0;
}

void audio_board_power(bool state)
{
    gpio_set_level(POWER_GPIO, state);
    gpio_set_level(SD_POWER_GPIO, !state);
}

void audio_board_poweroff()
{
    dac3100_deinit();
    audio_board_power(false);
    /* digital pads lose their level in deep sleep: keep peripherals and SD card switched off */
    gpio_hold_en(POWER_GPIO);
    gpio_hold_en(SD_POWER_GPIO);
    gpio_deep_sleep_hold_en();
    audio_board_deep_sleep();
}

/* The wake line (GPIO7, low = wake) is held low while the box is on the
   charging station, so with it as wake source the box woke up again right
   after going to sleep, every minute. In that case the box wakes on the ears
   instead and checks every few minutes (silently, see audio_board_sleep_check)
   whether it was taken off the charger, to arm the wake line again. */
#define CHARGER_CHECK_US (5ULL * 60 * 1000 * 1000)
/* which wake sources the last deep sleep used */
#define SLEEP_MODE_LINE 0x4C494E45u /* "LINE": wake line */
#define SLEEP_MODE_EARS 0x45415253u /* "EARS": ears + charger check timer */
static RTC_NOINIT_ATTR uint32_t sleep_mode;
/* additional timer wake (nightly update), 0 = none */
static uint64_t extra_wake_us = 0;

void audio_board_set_timer_wakeup(uint64_t us)
{
    extra_wake_us = us;
}

/* The charging station also gives the 5 V rail (GPIO8, ADC1 channel 7, 1:2
   divider: 2.5 V at the pin, 0 V on battery). On the station the wake line
   is not always held low: seen as wakes every few minutes while charging,
   the line only low for a moment. So the station is detected here too. */
static int charger_hint = -1; /* from the battery monitor before sleep, -1 unknown */
static int charger_raw = -1;  /* last direct reading (diagnosis) */

void audio_board_set_charger(bool present)
{
    charger_hint = present;
}

static bool charger_present(void)
{
    /* before sleep in normal operation the battery monitor has the ADC and
       told us its state (asking the ADC would only log an error) */
    if (charger_hint >= 0)
    {
        return charger_hint == 1;
    }
    adc_oneshot_unit_handle_t unit;
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&ucfg, &unit) != ESP_OK)
    {
        /* in use by the battery monitor: its last reading */
        return charger_hint == 1;
    }
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    int raw = 0;
    bool present = false;
    if (adc_oneshot_config_channel(unit, ADC_CHANNEL_7, &ccfg) == ESP_OK &&
        adc_oneshot_read(unit, ADC_CHANNEL_7, &raw) == ESP_OK)
    {
        /* about 3300 on the station; 2400 = about 1.8 V at the pin, 3.6 V rail */
        present = raw > 2400;
        charger_raw = raw;
    }
    adc_oneshot_del_unit(unit);
    return present;
}

static bool sleep_pin_low(gpio_num_t pin);

void audio_board_deep_sleep(void)
{
    if (!sleep_pin_low(WAKEUP_GPIO) && !charger_present())
    {
        sleep_mode = SLEEP_MODE_LINE;
        esp_sleep_enable_ext0_wakeup(WAKEUP_GPIO, 0);
        if (extra_wake_us)
        {
            esp_sleep_enable_timer_wakeup(extra_wake_us);
        }
    }
    else
    {
        sleep_mode = SLEEP_MODE_EARS;
        ESP_LOGI(TAG, "On the charger: wake on the ears, check again in %llu s", CHARGER_CHECK_US / 1000000);
        esp_sleep_enable_ext1_wakeup(BIT64(EAR_BIG_GPIO) | BIT64(EAR_SMALL_GPIO), ESP_EXT1_WAKEUP_ANY_LOW);
        /* keep the pull-ups of the (active low) ears in deep sleep */
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        rtc_gpio_pullup_en(EAR_BIG_GPIO);
        rtc_gpio_pulldown_dis(EAR_BIG_GPIO);
        rtc_gpio_pullup_en(EAR_SMALL_GPIO);
        rtc_gpio_pulldown_dis(EAR_SMALL_GPIO);
        esp_sleep_enable_timer_wakeup(extra_wake_us && extra_wake_us < CHARGER_CHECK_US ? extra_wake_us : CHARGER_CHECK_US);
    }
    esp_deep_sleep_start();
}

static bool sleep_pin_low(gpio_num_t pin)
{
    /* a wake source of the last sleep is still routed to the RTC domain */
    rtc_gpio_deinit(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_pullup_en(pin);
    esp_rom_delay_us(100);
    return gpio_get_level(pin) == 0;
}

/* what woke the box and how the lines were right after the wake (for the
   diagnosis of unexpected wakes, shown in Home Assistant) */
static char wake_info[80];

const char *audio_board_wake_info(void)
{
    return wake_info;
}

void audio_board_sleep_check(bool stay_awake_on_timer)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool line = false, big = false, small = false, station = false;
    if (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1)
    {
        line = sleep_pin_low(WAKEUP_GPIO);
        big = sleep_pin_low(EAR_BIG_GPIO);
        small = sleep_pin_low(EAR_SMALL_GPIO);
        station = charger_present();
        snprintf(wake_info, sizeof(wake_info), "%s; then line %s, ear %s, charger %s (%d)",
                 cause == ESP_SLEEP_WAKEUP_EXT0 ? "wake line" : "ear (on the charger)",
                 line ? "low" : "high", big ? (small ? "both" : "big") : (small ? "small" : "none"),
                 station ? "yes" : "no", charger_raw);
    }
    else if (cause == ESP_SLEEP_WAKEUP_TIMER)
    {
        snprintf(wake_info, sizeof(wake_info), "timer%s", stay_awake_on_timer ? " (nightly update)" : "");
    }
    /* woken by the wake line with no ear pressed, while it stays low or the
       station is there: the box was put on the charger (or the line moved
       while charging). Do not start, sleep on (waking on the ears). */
    bool put_on_charger = cause == ESP_SLEEP_WAKEUP_EXT0 && sleep_mode == SLEEP_MODE_LINE && (line || station) && !big && !small;
    if ((cause == ESP_SLEEP_WAKEUP_TIMER && !stay_awake_on_timer) || put_on_charger)
    {
        /* only the charger check: straight back to sleep, peripherals stay off
           (their pins are still held from the last sleep) */
        gpio_hold_en(POWER_GPIO);
        gpio_hold_en(SD_POWER_GPIO);
        gpio_deep_sleep_hold_en();
        audio_board_deep_sleep();
    }
}
