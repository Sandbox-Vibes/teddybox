/* Play MP3 file from SD Card

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"
#include "soc/rtc.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_partition.h"
#include "esp_event.h"
#include "esp_sleep.h"

#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "board.h"
#include "playback.h"
#include "wifi.h"
#include "webserver.h"
#include "accel.h"
#include "dac3100.h"
#include "ota.h"
#include "nfc.h"
#include "cloud.h"
#include "ledman.h"
#include "led.h"
#include "memdiag.h"
#include "settings.h"
#include "sfx.h"
#include "battery.h"
#include "bootsel.h"
#include "control.h"
#include "mqtt_ha.h"
#include "setup_portal.h"

#include "config.h"


static const char *TAG = "[TB]";
static wl_handle_t s_test_wl_handle;

typedef struct
{
    uint32_t rtc_magic;
    uint32_t volume;
    uint64_t nfc_uid;
    uint32_t play_position;
    uint32_t rtc_check;
} rtc_mem_t;

/* RTC_NOINIT: survives deep sleep and also software resets (OTA update,
   switching firmware, panic). RTC_DATA would be loaded again from the image
   on every boot except a deep sleep wake, losing the last tonie position.
   After power-on it holds garbage, magic and checksum catch that. */
static RTC_NOINIT_ATTR rtc_mem_t rtc_storage;

/* requests from other tasks, see control.h */
static volatile int ctrl_volume_request = -1;
static volatile bool ctrl_poweroff_request = false;
static volatile int ctrl_eq_request = -1;

void ctrl_request_speaker_eq(int mode)
{
    if (mode >= 0 && mode < SPEAKER_EQ_COUNT)
    {
        ctrl_eq_request = mode;
    }
}

int ctrl_get_speaker_eq(void)
{
    return dac3100_get_speaker_eq();
}

static volatile int ctrl_led_request = -1;

void ctrl_request_led_brightness(int level)
{
    if (level >= 0 && level <= LED_BRIGHTNESS_MAX)
    {
        ctrl_led_request = level;
    }
}

int ctrl_get_led_brightness(void)
{
    return led_get_brightness();
}

void ctrl_request_volume(int percent)
{
    int max = settings_get()->volume_max;
    ctrl_volume_request = percent < 0 ? 0 : (percent > max ? max : percent);
}

uint32_t ctrl_get_volume(void)
{
    return rtc_storage.volume;
}

void ctrl_request_poweroff(void)
{
    ctrl_poweroff_request = true;
}

void dir_list(const char *path)
{
    DIR *dir = opendir(path);
    while (true)
    {
        struct dirent *de = readdir(dir);
        if (!de)
        {
            break;
        }
        ESP_LOGI(TAG, "  - %s", de->d_name);
    }
    closedir(dir);
}

void print_all_tasks(void *params)
{
    TaskStatus_t *taskStatusArray;
    UBaseType_t taskCount;
    UBaseType_t i;
    size_t free_heap_size;

    while (1)
    {
        taskCount = uxTaskGetNumberOfTasks();
        taskStatusArray = pvPortMalloc(taskCount * sizeof(TaskStatus_t));

        if (taskStatusArray != NULL)
        {
            taskCount = uxTaskGetSystemState(taskStatusArray, taskCount, NULL);

            free_heap_size = esp_get_free_heap_size();
            ESP_LOGI(TAG, "Task Count:     %d", taskCount);
            ESP_LOGI(TAG, "Free Heap Size: %d", free_heap_size);
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "StackMark Name");

            for (i = 0; i < taskCount; i++)
            {
                ESP_LOGI(TAG, "%6d %s",
                         uxTaskGetStackHighWaterMark(taskStatusArray[i].xHandle),
                         taskStatusArray[i].pcTaskName);
            }

            vPortFree(taskStatusArray);
        }

        vTaskDelay(pdMS_TO_TICKS(5000)); // Wait for 5 seconds
    }
}

static uint32_t rotl32a(uint32_t x, uint32_t n)
{
    return (x << n) | (x >> (32 - n));
}

static uint32_t rtc_checksum_calc()
{
    uint8_t *buf = (uint8_t *)&rtc_storage;
    uint32_t len = sizeof(rtc_storage) - sizeof(uint32_t);
    uint32_t chk = 0x55AA5A5A;

    for (int pos = 0; pos < len; pos++)
    {
        chk ^= buf[pos];
        chk = rotl32a(chk, 3);
        chk += buf[pos];
        chk = rotl32a(chk, 7);
    }
    return chk;
}

static void rtc_checksum_update()
{
    rtc_storage.rtc_check = rtc_checksum_calc();
}

static bool rtc_checksum_valid()
{
    return rtc_storage.rtc_check == rtc_checksum_calc();
}

void app_main(void)
{
    /* woken only to see whether the box is still on the charger */
    audio_board_sleep_check();

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    memdiag_init();
    {
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        /* EXT0 is the wake line, or the small ear while on the charger */
        ESP_LOGI(TAG, "Wake cause: %s", cause == ESP_SLEEP_WAKEUP_EXT0   ? "wake line / small ear"
                                        : cause == ESP_SLEEP_WAKEUP_EXT1 ? "big ear"
                                        : cause == ESP_SLEEP_WAKEUP_UNDEFINED ? "power on / reset"
                                                                           : "other");
    }

    /* The NVS partition belongs to the original firmware (WiFi, OTA slot table,
       hardware revision, settings). Never erase it, even if it looks unusable. */
    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS init failed (%s), continuing without stored settings", esp_err_to_name(ret));
    }
    settings_init();
    bootsel_init();

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    ESP_LOGI(TAG, "Board init");
    audio_board_handle_t board_handle = audio_board_init();
    dac3100_set_speaker_eq(settings_get()->speaker_eq);
    led_set_brightness(settings_get()->led_brightness);
    ledman_init();

    /* big ear held while switching on: open the setup page */
    bool setup_requested = false;
    if (audio_board_ear_big())
    {
        int held = 0;
        while (audio_board_ear_big() && held < SETUP_EAR_HOLD_MS)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            held += 100;
        }
        setup_requested = held >= SETUP_EAR_HOLD_MS;
    }

    ESP_LOGI(TAG, "Mount sdcard");
    audio_board_sdcard_init(set, SD_MODE_4_LINE);

    ESP_LOGI(TAG, "Mount assets");
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 3};
    if (esp_vfs_fat_spiflash_mount("/spiflash", NULL, &mount_config, &s_test_wl_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount assets partition");
    }

    /* first boot: no settings file yet */
    bool setup_mode = setup_requested || !settings_ini_exists();
    settings_load_ini();
    sfx_install(settings_get()->lang_id);

    ESP_LOGI(TAG, "Start codec chip");
    audio_hal_ctrl_codec(audio_board_get_hal(), AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "Start handlers");

    pb_init(set);

    // xTaskCreate(print_all_tasks, "print_all_tasks", 4096, NULL, 5, NULL);

    if (rtc_storage.rtc_magic != 0xDEADC0DE || !rtc_checksum_valid())
    {
        ESP_LOGE(TAG, "RTC memory corrupted, reinit");
        memset(&rtc_storage, 0x00, sizeof(rtc_storage));
        rtc_storage.rtc_magic = 0xDEADC0DE;
        rtc_storage.volume = settings_get()->volume_default;
        rtc_storage.play_position = 0;
        rtc_checksum_update();
    }
    else if (rtc_storage.nfc_uid)
    {
        ESP_LOGI(TAG, "Inform playback handler UID %16llX / %d", rtc_storage.nfc_uid, rtc_storage.play_position);
        pb_set_last(rtc_storage.nfc_uid, rtc_storage.play_position);
    }

    if (rtc_storage.volume > settings_get()->volume_max)
    {
        rtc_storage.volume = settings_get()->volume_max;
    }
    audio_hal_set_volume(audio_board_get_hal(), rtc_storage.volume);

    dac3100_set_mute(true);

    if (setup_mode)
    {
        ESP_LOGI(TAG, "Setup mode%s", setup_requested ? " (big ear held)" : " (no settings yet)");
        /* the setup mode must be visible even with the LED switched off */
        if (led_get_brightness() == 0)
        {
            led_set_brightness(1);
        }
        ledman_change(SETUP_LED_PATTERN);
        /* "Hallo, nicht erschrecken ... brauche ich Hilfe bei der Installation" */
        dac3100_set_mute(false);
        pb_play_default(CONTENT_DEFAULT_INSTALLATION);
        /* returns only if nobody used it, restarts after saving */
        setup_portal_run();
        goto shutdown;
    }

    pb_play_default(CONTENT_DEFAULT_STARTUP);

    bool ear_big_prev = false;
    bool ear_small_prev = false;

    accel_init(board_handle);
    battery_init();
    wifi_init();
    nfc_init();
    cloud_init();
    mqtt_ha_init();
    /* already too much memory consumption, do not enable by default */
    // www_init();
    /* updates come through the setup page, see ota.h */

    int64_t last_activity_time = esp_timer_get_time();
    int64_t remute_time = 0;
    int64_t both_ears_since = 0;
    /* a new firmware runs on probation until it confirms itself, see ota.h */
    const int64_t confirm_time = esp_timer_get_time() + OTA_CONFIRM_AFTER_US;
    bool confirmed = false;
    bool low_battery_warned = false;
    const int64_t poweroff_timeout = (int64_t)settings_get()->idle_timeout_s * 1000000LL;
    int64_t memdiag_next = esp_timer_get_time() + MEMDIAG_INTERVAL_US;

    while (1)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        int64_t cur_time = esp_timer_get_time();

        if (cur_time >= memdiag_next)
        {
            memdiag_next = cur_time + MEMDIAG_INTERVAL_US;
            memdiag_log_tasks();
        }

        bool ear_big = audio_board_ear_big();
        bool ear_small = audio_board_ear_small();

        if (ear_big && !ear_big_prev)
        {
            dac3100_set_mute(false);
            int vol_up = dac3100_volume_step(rtc_storage.volume, 1);
            if (vol_up >= 0 && vol_up <= settings_get()->volume_max)
            {
                ESP_LOGI(TAG, "Volume up");
                rtc_storage.volume = vol_up;
                audio_hal_set_volume(audio_board_get_hal(), rtc_storage.volume);
                dac3100_beep(0, 0x140);
            }
            else
            {
                ESP_LOGI(TAG, "Volume up (limit)");
                dac3100_beep(1, 0x140);
            }
            remute_time = cur_time + 800;
        }

        if (ear_small && !ear_small_prev)
        {
            dac3100_set_mute(false);
            int vol_down = dac3100_volume_step(rtc_storage.volume, -1);
            if (vol_down >= 0)
            {
                ESP_LOGI(TAG, "Volume down");
                rtc_storage.volume = vol_down;
                audio_hal_set_volume(audio_board_get_hal(), rtc_storage.volume);
                dac3100_beep(2, 0x140);
            }
            else
            {
                ESP_LOGI(TAG, "Volume down (limit)");
                dac3100_beep(3, 0x140);
            }
            remute_time = cur_time + 800;
        }

        if (!confirmed && cur_time > confirm_time)
        {
            ota_confirm_running();
            confirmed = true;
        }

        int vol_req = ctrl_volume_request;
        if (vol_req >= 0)
        {
            ctrl_volume_request = -1;
            ESP_LOGI(TAG, "Volume set to %d", vol_req);
            rtc_storage.volume = vol_req;
            audio_hal_set_volume(audio_board_get_hal(), rtc_storage.volume);
            last_activity_time = cur_time;
        }
        int led_req = ctrl_led_request;
        if (led_req >= 0)
        {
            ctrl_led_request = -1;
            ESP_LOGI(TAG, "LED brightness: %d", led_req);
            led_set_brightness(led_req);
            if (settings_get()->led_brightness != led_req)
            {
                settings_get_mutable()->led_brightness = led_req;
                if (settings_save_ini(SETTINGS_INI_PATH) != 0)
                {
                    ESP_LOGW(TAG, "Could not save LED brightness");
                }
            }
        }

        int eq_req = ctrl_eq_request;
        if (eq_req >= 0)
        {
            ctrl_eq_request = -1;
            ESP_LOGI(TAG, "Speaker EQ: %s", speaker_eq_name(eq_req));
            dac3100_set_speaker_eq(eq_req);
            if (settings_get()->speaker_eq != eq_req)
            {
                settings_get_mutable()->speaker_eq = eq_req;
                if (settings_save_ini(SETTINGS_INI_PATH) != 0)
                {
                    ESP_LOGW(TAG, "Could not save speaker EQ");
                }
            }
        }
        if (ctrl_poweroff_request)
        {
            ESP_LOGI(TAG, "Power off requested");
            break;
        }

        if (remute_time != 0 && cur_time > remute_time)
        {
            remute_time = 0;
            dac3100_set_mute(!pb_is_playing());
        }

        if (pb_is_playing() || ear_big || ear_small)
        {
            last_activity_time = cur_time;
        }

        if (pb_is_playing())
        {
            rtc_storage.nfc_uid = pb_get_current_uid();
            rtc_storage.play_position = pb_get_play_position();
        }

        /* hold both ears for 5 seconds: reboot into the original firmware */
        if (ear_big && ear_small)
        {
            if (!both_ears_since)
            {
                both_ears_since = cur_time;
            }
            else if (cur_time - both_ears_since > BOOT_OFW_HOLD_TIME)
            {
                ESP_LOGW(TAG, "Both ears held, switching to original firmware");
                ledman_change("fadeblink blue");
                pb_stop();
                rtc_checksum_update();
                bootsel_boot_original();
                /* only returns on error */
                ledman_change("failed");
                both_ears_since = 0;
            }
        }
        else
        {
            both_ears_since = 0;
        }

        battery_state_t bat = battery_get_state();
        if (bat == BATTERY_CRITICAL)
        {
            ESP_LOGW(TAG, "Battery critical (%lu mV), shutting down", (unsigned long)battery_get_mv());
            pb_play_default(CONTENT_DEFAULT_LOW_BATTERY_SHUTDOWN);
            /* give the announcement time to start and finish */
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            for (int i = 0; i < 80 && pb_is_playing(); i++)
            {
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            break;
        }
        if (bat == BATTERY_LOW && !low_battery_warned && !pb_is_playing())
        {
            /* do not interrupt a story, warn as soon as the box is idle */
            ESP_LOGW(TAG, "Battery low (%lu mV)", (unsigned long)battery_get_mv());
            pb_play_default(CONTENT_DEFAULT_LOW_BATTERY_WARN);
            low_battery_warned = true;
        }
        else if (bat == BATTERY_OK)
        {
            low_battery_warned = false;
        }

        if ((cur_time - last_activity_time) > poweroff_timeout)
        {
            break;
        }

        ear_big_prev = ear_big;
        ear_small_prev = ear_small;

        /* keep it valid at all times, a software reset can come anytime */
        rtc_checksum_update();
    }

    mqtt_ha_shutdown();
shutdown:
    ledman_change("poweroff");
    audio_board_sdcard_unmount();
    rtc_checksum_update();

    ESP_LOGI(TAG, "Poweroff");
    vTaskDelay(500 / portTICK_PERIOD_MS);
    audio_board_poweroff();

    ESP_LOGE(TAG, "back, quite unexpected...");
}
