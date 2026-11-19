/*
 * Battery and charger monitoring.
 *
 * Hardware (see tonies wiki, ESP32 pinout):
 *   GPIO9 = ADC1_CH8: battery voltage through R57/R58 100k/33k divider
 *   GPIO8 = ADC1_CH7: charger 5V rail through R75/R72 100k/100k divider
 *
 * Thresholds come from the original firmware's NVS (TB_SETTINGS):
 *   BAT_CRITICAL (3600 mV), BAT_LOW_LEVEL_D (3670 mV), BAT_LOW_LEVEL_U (3700 mV)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"

#include "battery.h"
#include "settings.h"
#include "playback.h"

static const char *TAG = "Battery";

#define VBAT_CHANNEL ADC1_CHANNEL_8 /* GPIO9 */
#define VCHG_CHANNEL ADC1_CHANNEL_7 /* GPIO8 */

/* divider ratios multiplied by 1000 */
#define VBAT_RATIO_X1000 ((100 + 33) * 1000 / 33)
#define VCHG_RATIO_X1000 2000

#define CHARGER_PRESENT_MV 4000
#define SAMPLE_INTERVAL_MS 2000
#define SAMPLES_PER_READ 16
/* consecutive readings below critical before we report it, filters load peaks */
#define CRITICAL_CONFIRM 3
#define IMPLAUSIBLE_MV 2800

static esp_adc_cal_characteristics_t cal_vbat;
static esp_adc_cal_characteristics_t cal_vchg;

static volatile uint32_t vbat_mv = 0;
static volatile bool charging = false;
static volatile battery_state_t state = BATTERY_UNKNOWN;
/* estimated voltage without the load of the speaker amplifier, for the
   percentage (0 = unknown, e.g. while charging) */
static volatile uint32_t rest_mv = 0;

/* samples after a play/pause change until the filtered value has settled
   (24 s; the IIR filter below has then moved 97 % of the way) */
#define SETTLE_SAMPLES 12
#define MAX_SAG_MV 250

/* While idle the measured voltage is the rest voltage. While playing it sags
   with the amplifier load: the sag is measured at the last change between
   playing and idle (voltage before vs. after settling) and added back. */
static void update_rest(uint32_t filtered)
{
    static bool was_playing = false;
    static int settle = 0;
    static uint32_t ref_mv = 0;
    static uint32_t sag_mv = 0;
    static uint32_t last_mv = 0;

    if (charging)
    {
        /* the charger lifts the voltage; start over when it is removed */
        rest_mv = 0;
        ref_mv = 0;
        settle = 0;
        last_mv = 0;
        return;
    }

    bool playing = pb_is_playing();
    uint32_t prev_mv = last_mv;
    last_mv = filtered;
    if (playing != was_playing)
    {
        /* idle -> playing: reference is the rest voltage; playing -> idle: the
           loaded one, from before this sample (the filter already moves) */
        ref_mv = playing ? rest_mv : prev_mv;
        settle = 0;
        was_playing = playing;
    }
    if (settle < SETTLE_SAMPLES)
    {
        settle++;
        if (settle == SETTLE_SAMPLES && ref_mv)
        {
            int32_t sag = playing ? (int32_t)ref_mv - (int32_t)filtered : (int32_t)filtered - (int32_t)ref_mv;
            sag_mv = sag < 0 ? 0 : (sag > MAX_SAG_MV ? MAX_SAG_MV : sag);
            ESP_LOGI(TAG, "Voltage drop while playing: %lu mV", (unsigned long)sag_mv);
        }
        else if (rest_mv)
        {
            return; /* keep the last estimate while the voltage moves */
        }
    }
    rest_mv = playing ? filtered + sag_mv : filtered;
}

static uint32_t read_mv(adc1_channel_t ch, esp_adc_cal_characteristics_t *cal, uint32_t ratio_x1000)
{
    uint32_t sum = 0;
    for (int i = 0; i < SAMPLES_PER_READ; i++)
    {
        sum += adc1_get_raw(ch);
    }
    uint32_t pin_mv = esp_adc_cal_raw_to_voltage(sum / SAMPLES_PER_READ, cal);
    return pin_mv * ratio_x1000 / 1000;
}

static void battery_task(void *arg)
{
    const settings_t *s = settings_get();
    int below_critical = 0;
    uint32_t filtered = 0;

    while (true)
    {
        uint32_t mv = read_mv(VBAT_CHANNEL, &cal_vbat, VBAT_RATIO_X1000);
        uint32_t chg = read_mv(VCHG_CHANNEL, &cal_vchg, VCHG_RATIO_X1000);

        /* simple IIR filter, initialize with the first value */
        filtered = filtered ? (filtered * 3 + mv) / 4 : mv;
        vbat_mv = filtered;

        bool was_charging = charging;
        charging = chg > CHARGER_PRESENT_MV;
        if (charging != was_charging)
        {
            ESP_LOGI(TAG, "Charger %s (%lu mV)", charging ? "connected" : "removed", (unsigned long)chg);
        }

        battery_state_t prev = state;
        if (filtered < IMPLAUSIBLE_MV)
        {
            /* 3 NiMH cells never read this low while the ESP32 still runs:
               measurement problem or no battery, so never shut down because of it */
            state = BATTERY_UNKNOWN;
            below_critical = 0;
        }
        else if (charging)
        {
            /* voltage is lifted while charging, never warn on the station */
            state = BATTERY_OK;
            below_critical = 0;
        }
        else
        {
            below_critical = (filtered < s->bat_critical_mv) ? below_critical + 1 : 0;

            if (below_critical >= CRITICAL_CONFIRM)
            {
                state = BATTERY_CRITICAL;
            }
            else if (filtered < s->bat_low_down_mv)
            {
                if (state != BATTERY_CRITICAL)
                {
                    state = BATTERY_LOW;
                }
            }
            else if (filtered > s->bat_low_up_mv || state == BATTERY_UNKNOWN)
            {
                state = BATTERY_OK;
            }
        }

        if (filtered >= IMPLAUSIBLE_MV)
        {
            update_rest(filtered);
        }

        if (state != prev)
        {
            ESP_LOGI(TAG, "%s -> %s at %lu mV", battery_state_name(prev), battery_state_name(state), (unsigned long)filtered);
        }

        vTaskDelay(SAMPLE_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

void battery_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    adc1_config_width(ADC_WIDTH_BIT_12);
    /* battery: max ~4.5V / 4.03 = 1.12V at the pin -> 6dB (up to ~1.75V) */
    adc1_config_channel_atten(VBAT_CHANNEL, ADC_ATTEN_DB_6);
    /* charger: 5V / 2 = 2.5V at the pin -> 11dB (up to ~3.1V) */
    adc1_config_channel_atten(VCHG_CHANNEL, ADC_ATTEN_DB_11);

    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_6, ADC_WIDTH_BIT_12, 1100, &cal_vbat);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &cal_vchg);

    xTaskCreate(&battery_task, "[TB] battery", 2560, NULL, 4, NULL);
}

uint32_t battery_get_mv(void)
{
    return vbat_mv;
}

uint32_t battery_get_rest_mv(void)
{
    return rest_mv;
}

bool battery_is_charging(void)
{
    return charging;
}

battery_state_t battery_get_state(void)
{
    return state;
}

const char *battery_state_name(battery_state_t s)
{
    switch (s)
    {
    case BATTERY_OK:
        return "ok";
    case BATTERY_LOW:
        return "low";
    case BATTERY_CRITICAL:
        return "critical";
    default:
        return "unknown";
    }
}
