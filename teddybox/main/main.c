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
#include "i18n.h"
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
#include "accel.h"
#include "ear_gestures.h"
#include "taskwatch.h"
#include "dac3100.h"
#include "ota.h"
#include "maint.h"
#include "esp_pm.h"
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
#include "rtnl.h"
#include "crashlog.h"
#include "sleep_timer.h"
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
    uint32_t audio_id; /* content of play_position (a new episode starts over) */
    uint32_t rtc_check; /* over everything before it */
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

static volatile bool ctrl_motion_save = false;

void ctrl_request_motion(int slap, int tilt, int reverse)
{
    /* the accel task reads the settings in every loop */
    settings_t *s = settings_get_mutable();
    if (slap >= 0 && slap <= 5)
    {
        s->slap_level = slap;
    }
    if (tilt >= 0 && tilt <= 5)
    {
        s->tilt_level = tilt;
    }
    if (reverse == 0 || reverse == 1)
    {
        s->slap_reverse = reverse;
    }
    ctrl_motion_save = true;
}

int ctrl_volume_max(void)
{
    return pb_headphones_connected() ? settings_get()->volume_max_hdp : settings_get()->volume_max;
}

static volatile int ctrl_vmax_spk_request = -1, ctrl_vmax_hdp_request = -1;

void ctrl_request_volume_max(int speaker, int headphones)
{
    if (speaker >= 10 && speaker <= 100)
    {
        ctrl_vmax_spk_request = speaker;
    }
    if (headphones >= 10 && headphones <= 100)
    {
        ctrl_vmax_hdp_request = headphones;
    }
}

void ctrl_request_volume(int percent)
{
    int max = ctrl_volume_max();
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

static volatile bool ctrl_boot_original_request = false;

void ctrl_request_boot_original(void)
{
    ctrl_boot_original_request = true;
}

static void rtc_checksum_update(void);

/* Home Assistant "Start setup": restart into setup mode; survives the
   software reset, cleared when read */
#define SETUP_REQUEST_MAGIC 0x53455455u /* "SETU" */
static RTC_NOINIT_ATTR uint32_t setup_request_magic;
static volatile bool ctrl_setup_request = false;

void ctrl_request_setup(void)
{
    ctrl_setup_request = true;
}

/* feedback for the main loop (EG_RESTART_ARMED, EG_OFW_ARMED, EG_CANCELLED) */
static volatile int ears_signal = EG_NONE;

/* gestures with both ears as in the original firmware / Hackiebox CFW, see
   ear_gestures.h. Own small task so that restart and switching to the
   original firmware still work when the main loop hangs (idea from the
   jcarletto27/teddybox fork). Without a working accelerometer the long
   gestures also work upright. */
static void ears_task(void *arg)
{
    ear_gesture_t g = {0};
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        int64_t now = esp_timer_get_time();
        eg_action_t a = ear_gesture_step(&g, now, audio_board_ear_big(), audio_board_ear_small(),
                                         !accel_working() || accel_upside_down());
        switch (a)
        {
        case EG_RESTART_ARMED:
            ESP_LOGW(TAG, "Both ears held upside down 10 s: restart when let go (hold on to 20 s: original firmware)");
            ears_signal = a;
            break;
        case EG_OFW_ARMED:
            ESP_LOGW(TAG, "Both ears held upside down 20 s: original firmware when let go");
            ears_signal = a;
            break;
        case EG_CANCELLED:
            ESP_LOGW(TAG, "Both ears held too long or box turned: nothing");
            ears_signal = a;
            break;
        case EG_RESTART:
            ESP_LOGW(TAG, "Restart (both ears upside down)");
            rtc_checksum_update();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
            break;
        case EG_OFW:
            ESP_LOGW(TAG, "Switching to the original firmware (both ears upside down)");
            pb_stop();
            rtc_checksum_update();
            bootsel_boot_original();
            /* only returns on error */
            ledman_change("failed");
            break;
        case EG_POWEROFF:
            if (settings_get()->ears_poweroff)
            {
                ESP_LOGI(TAG, "Both ears pressed briefly: power off");
                ctrl_request_poweroff();
            }
            break;
        default:
            break;
        }
    }
}

/* sleep timer, driven by the main loop */
#define SLEEP_TIMER_GESTURE_MIN 30
#define SLEEP_TIMER_HOLD_US (2 * 1000000LL)
static sleep_timer_t sleep_timer;
static volatile int ctrl_sleep_request = -1;

void ctrl_request_sleep_timer(int minutes)
{
    ctrl_sleep_request = minutes < 0 ? 0 : minutes;
}

int ctrl_get_sleep_timer(void)
{
    return sleep_timer_remaining_min(&sleep_timer, esp_timer_get_time());
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
    /* everything before the checksum (sizeof - 4 would include it once the
       struct has padding at the end, as with the audio ID) */
    uint32_t len = offsetof(rtc_mem_t, rtc_check);
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
    /* a timer wake is the charger check or the nightly update; everything
       else than a due update goes straight back to sleep */
    maint_clock_wake();
    const bool maint_mode = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && maint_due();
    audio_board_set_timer_wakeup(maint_sleep_us());
    audio_board_sleep_check(maint_mode);

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    memdiag_init();
    {
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        ESP_LOGI(TAG, "Wake cause: %s", cause == ESP_SLEEP_WAKEUP_EXT0   ? "wake line"
                                        : cause == ESP_SLEEP_WAKEUP_EXT1 ? "ear (on the charger)"
                                        : cause == ESP_SLEEP_WAKEUP_TIMER ? "timer (nightly update)"
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
    /* or the Home Assistant button "Start setup" before the restart */
    if (setup_request_magic == SETUP_REQUEST_MAGIC && esp_reset_reason() == ESP_RST_SW)
    {
        setup_requested = true;
    }
    setup_request_magic = 0;

    ESP_LOGI(TAG, "Mount sdcard");
    audio_board_sdcard_init(set, SD_MODE_4_LINE);

    /* A just installed firmware first checks the hardware: a new framework
       version could break a driver (SD, I2S/DAC, I2C, SPI). Then it goes back
       to the previous firmware instead of confirming itself. */
    {
        char failed[48];
        if (!audio_board_selftest(failed, sizeof(failed)))
        {
            ESP_LOGE(TAG, "Hardware check failed: %s", failed);
            if (ota_on_probation())
            {
                ota_reject_running(failed);
            }
        }
    }

    ESP_LOGI(TAG, "Mount assets");
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 3};
    if (esp_vfs_fat_spiflash_mount_rw_wl("/spiflash", NULL, &mount_config, &s_test_wl_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount assets partition");
    }

    /* first boot: no settings file yet */
    bool setup_mode = setup_requested || !settings_ini_exists();
    settings_load_ini();
    setenv("TZ", settings_get()->timezone, 1);
    tzset();
    maint_log_status();
    crashlog_check();
    /* fixed CPU clock from teddybox.ini (power management only for that, no light sleep) */
    {
        int mhz = settings_get()->cpu_mhz;
        if (!settings_get()->decoder_celt && mhz < 240)
        {
            /* the ESP-ADF decoder stuttered at 160 MHz on some tonies */
            ESP_LOGW(TAG, "decoder = adf needs 240 MHz, ignoring cpu_mhz = %d", mhz);
            mhz = 240;
        }
        esp_pm_config_t pm = {.max_freq_mhz = mhz, .min_freq_mhz = mhz, .light_sleep_enable = false};
        esp_err_t err = esp_pm_configure(&pm);
        ESP_LOGI(TAG, "CPU clock %d MHz%s", mhz, err == ESP_OK ? "" : " (failed)");
    }
    memdiag_enable(settings_get()->mem_log);
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
        pb_set_last(rtc_storage.nfc_uid, rtc_storage.play_position, rtc_storage.audio_id);
    }

    if (rtc_storage.volume > ctrl_volume_max())
    {
        rtc_storage.volume = ctrl_volume_max();
        rtc_checksum_update();
    }
    audio_hal_set_volume(audio_board_get_hal(), rtc_storage.volume);

    dac3100_set_mute(true);

    if (setup_mode)
    {
        ESP_LOGI(TAG, "Setup mode%s", setup_requested ? " (requested)" : " (no settings yet)");
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

    if (maint_mode)
    {
        /* silent: no sound, LED off, no NFC (a tonie on the box is not played) */
        ESP_LOGI(TAG, "Nightly update");
        ledman_change("off");
        battery_init();
        wifi_init();
        cloud_init();
        mqtt_ha_init();
        maint_guard_start();
        maint_run();
        /* Home Assistant shows the result (WiFi needs a few seconds, the run
           may have ended right away, e.g. battery too low) */
        maint_stage(L("result to Home Assistant", "Ergebnis an Home Assistant"));
        if (!mqtt_ha_flush(30000) && settings_get()->mqtt_host[0])
        {
            ESP_LOGW(TAG, "Result not sent to Home Assistant (MQTT not connected)");
        }
        mqtt_ha_shutdown();
        maint_stage(L("switching off", "Ausschalten"));
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
    rtnl_init();
    mqtt_ha_init();
    /* already too much memory consumption, do not enable by default */
    /* updates come through the setup page, see ota.h */

    int64_t last_activity_time = esp_timer_get_time();
    int64_t remute_time = 0;
    xTaskCreatePinnedToCore(ears_task, "[TB] ears", 3072, NULL, 4, NULL, tskNO_AFFINITY);
    /* small ear held alone: sleep timer on/off */
    int64_t small_ear_since = 0;
    uint32_t small_ear_volume = 0;
    bool small_ear_used = false;
    /* volume applied while the sleep timer fades, -1 = none */
    int sleep_fade_volume = -1;
    uint32_t sleep_fade_base = 0;
    /* a new firmware runs on probation until it confirms itself, see ota.h */
    const int64_t confirm_time = esp_timer_get_time() + OTA_CONFIRM_AFTER_US;
    bool confirmed = false;
    bool low_battery_warned = false;
    const int64_t poweroff_timeout = (int64_t)settings_get()->idle_timeout_s * 1000000LL;
    int64_t memdiag_next = esp_timer_get_time() + MEMDIAG_INTERVAL_US;

    int tw_main = taskwatch_add("main loop", 30000);
    while (1)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        taskwatch_feed(tw_main);
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
            rtnl_event(RTNL3_EAR_BIG);
            dac3100_set_mute(false);
            int vol_up = dac3100_volume_step(rtc_storage.volume, 1);
            if (vol_up >= 0 && vol_up <= ctrl_volume_max())
            {
                ESP_LOGI(TAG, "Volume up");
                rtc_storage.volume = vol_up;
                audio_hal_set_volume(audio_board_get_hal(), rtc_storage.volume);
                rtnl_volume(rtc_storage.volume);
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
            small_ear_since = cur_time;
            small_ear_volume = rtc_storage.volume;
            small_ear_used = false;
            rtnl_event(RTNL3_EAR_SMALL);
            dac3100_set_mute(false);
            int vol_down = dac3100_volume_step(rtc_storage.volume, -1);
            if (vol_down >= 0)
            {
                ESP_LOGI(TAG, "Volume down");
                rtc_storage.volume = vol_down;
                audio_hal_set_volume(audio_board_get_hal(), rtc_storage.volume);
                rtnl_volume(rtc_storage.volume);
                dac3100_beep(2, 0x140);
            }
            else
            {
                ESP_LOGI(TAG, "Volume down (limit)");
                dac3100_beep(3, 0x140);
            }
            remute_time = cur_time + 800;
        }

        /* hold the small ear alone for 2 s: sleep timer on (30 min) or off;
           the volume step of the press is taken back */
        if (ear_small && !ear_big && !small_ear_used && small_ear_since &&
            cur_time - small_ear_since >= SLEEP_TIMER_HOLD_US)
        {
            small_ear_used = true;
            rtc_storage.volume = small_ear_volume;
            audio_hal_set_volume(audio_board_get_hal(), rtc_storage.volume);
            ctrl_request_sleep_timer(sleep_timer_active(&sleep_timer) ? 0 : SLEEP_TIMER_GESTURE_MIN);
        }
        if (!ear_small || ear_big)
        {
            small_ear_since = 0;
        }

        int sleep_req = ctrl_sleep_request;
        if (sleep_req >= 0)
        {
            ctrl_sleep_request = -1;
            bool was_active = sleep_timer_active(&sleep_timer);
            sleep_timer_set(&sleep_timer, cur_time, sleep_req);
            dac3100_set_mute(false);
            if (sleep_req)
            {
                ESP_LOGI(TAG, "Sleep timer: %d min", sleep_timer_remaining_min(&sleep_timer, cur_time));
                dac3100_beep(1, 0x400);
            }
            else if (was_active)
            {
                ESP_LOGI(TAG, "Sleep timer off");
                dac3100_beep(3, 0x400);
            }
            remute_time = cur_time + 1000000;
            if (sleep_fade_volume >= 0)
            {
                /* fade stopped (timer off or restarted): full volume again */
                sleep_fade_volume = -1;
                audio_hal_set_volume(audio_board_get_hal(), rtc_storage.volume);
            }
        }
        if (sleep_timer_active(&sleep_timer))
        {
            int level = sleep_timer_level(&sleep_timer, cur_time);
            if (level < 0)
            {
                ESP_LOGI(TAG, "Sleep timer: time is up, switching off");
                pb_stop();
                break;
            }
            if (level < 1000)
            {
                int vol = (int)(rtc_storage.volume * level / 1000);
                /* also when the volume was changed meanwhile (ears, Home Assistant) */
                if (vol != sleep_fade_volume || rtc_storage.volume != sleep_fade_base)
                {
                    sleep_fade_volume = vol;
                    sleep_fade_base = rtc_storage.volume;
                    audio_hal_set_volume(audio_board_get_hal(), vol);
                }
            }
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
            rtnl_volume(rtc_storage.volume);
            last_activity_time = cur_time;
        }
        int vmax_spk = ctrl_vmax_spk_request, vmax_hdp = ctrl_vmax_hdp_request;
        if (vmax_spk >= 0 || vmax_hdp >= 0)
        {
            ctrl_vmax_spk_request = ctrl_vmax_hdp_request = -1;
            settings_t *ms = settings_get_mutable();
            if (vmax_spk >= 0)
            {
                ms->volume_max = vmax_spk;
            }
            if (vmax_hdp >= 0)
            {
                ms->volume_max_hdp = vmax_hdp;
            }
            if (ms->volume_default > ms->volume_max)
            {
                ms->volume_default = ms->volume_max;
            }
            ESP_LOGI(TAG, "Max volume: speaker %u %%, headphones %u %%", ms->volume_max, ms->volume_max_hdp);
            if (settings_save_ini(SETTINGS_INI_PATH) != 0)
            {
                ESP_LOGW(TAG, "Could not save the max volume");
            }
        }
        /* lower limit (new limit, headphones plugged in): turn down */
        if (rtc_storage.volume > (uint32_t)ctrl_volume_max())
        {
            rtc_storage.volume = ctrl_volume_max();
            ESP_LOGI(TAG, "Volume lowered to the limit %u %%", (unsigned)rtc_storage.volume);
            audio_hal_set_volume(audio_board_get_hal(), rtc_storage.volume);
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

        if (ctrl_motion_save)
        {
            ctrl_motion_save = false;
            ESP_LOGI(TAG, "Slap %d%s, tilt %d", settings_get()->slap_level, settings_get()->slap_reverse ? " (reversed)" : "",
                     settings_get()->tilt_level);
            if (settings_save_ini(SETTINGS_INI_PATH) != 0)
            {
                ESP_LOGW(TAG, "Could not save slap/tilt settings");
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

        /* also after the playback ended: a tonie played to the end continues
           from the start, after deep sleep too */
        if (pb_get_current_uid())
        {
            rtc_storage.nfc_uid = pb_get_current_uid();
            rtc_storage.play_position = pb_get_resume_position();
            rtc_storage.audio_id = pb_get_resume_audio_id();
        }

        /* feedback for the gestures with both ears; they are evaluated by
           ears_task, so restart and original firmware work even when this
           loop hangs */
        int ears_sig = ears_signal;
        if (ears_sig != EG_NONE)
        {
            ears_signal = EG_NONE;
            if (ears_sig == EG_RESTART_ARMED || ears_sig == EG_OFW_ARMED)
            {
                if (ears_sig == EG_RESTART_ARMED)
                {
                    pb_stop();
                }
                else
                {
                    ledman_change("fadeblink blue");
                }
                dac3100_set_mute(false);
                dac3100_beep(1, 0x400);
                remute_time = cur_time + 1000000;
            }
            else if (ears_sig == EG_CANCELLED)
            {
                ledman_change("idle");
            }
        }
        if (ctrl_setup_request)
        {
            ctrl_setup_request = false;
            ESP_LOGW(TAG, "Restarting into setup mode (Home Assistant)");
            pb_stop();
            rtc_checksum_update();
            setup_request_magic = SETUP_REQUEST_MAGIC;
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        if (ctrl_boot_original_request)
        {
            ctrl_boot_original_request = false;
            ESP_LOGW(TAG, "Switching to the original firmware");
            pb_stop();
            rtc_checksum_update();
            bootsel_boot_original();
            /* only returns on error */
            ledman_change("failed");
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
    /* plan the nightly update (needs the clock, set from teddyCloud) */
    maint_schedule_next();
    audio_board_set_timer_wakeup(maint_sleep_us());
    maint_clock_sleep();
    ledman_change("poweroff");
    audio_board_sdcard_unmount();
    rtc_checksum_update();

    ESP_LOGI(TAG, "Poweroff");
    maint_run_end();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    audio_board_poweroff();

    ESP_LOGE(TAG, "back, quite unexpected...");
}
