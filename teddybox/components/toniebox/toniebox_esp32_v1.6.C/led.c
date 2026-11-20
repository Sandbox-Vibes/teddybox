
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/ledc.h"

#include "board.h"

static const char *TAG = "LED";

#define LED_DUTY_BITS LEDC_TIMER_13_BIT

/* brightness levels 0..LED_BRIGHTNESS_MAX as share of the PWM duty; the eye
   is far from linear, equal steps would look almost the same at the top */
static const float led_levels[LED_BRIGHTNESS_MAX + 1] = {0.0f, 0.10f, 0.35f, 1.0f};
static int led_level = LED_BRIGHTNESS_MAX;
static float led_last_r, led_last_g, led_last_b;

void led_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    /* IDF 5: LEDC instead of the legacy MCPWM driver; 1 kHz like before */
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LED_DUTY_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const int gpio[3] = {LED_RED_GPIO, LED_GREEN_GPIO, LED_BLUE_GPIO};
    for (int i = 0; i < 3; i++)
    {
        ledc_channel_config_t ch = {
            .gpio_num = gpio[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)i,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }

    led_set_rgb(0, 0, 0);
}

/* pct: duty in percent, 0..100 */
void led_set(led_t led, float pct)
{
    ledc_channel_t ch = LEDC_CHANNEL_0;
    switch (led)
    {
    case LED_RED:
        ch = LEDC_CHANNEL_0;
        break;
    case LED_GREEN:
        ch = LEDC_CHANNEL_1;
        break;
    case LED_BLUE:
        ch = LEDC_CHANNEL_2;
        break;
    }
    if (pct < 0)
    {
        pct = 0;
    }
    if (pct > 100)
    {
        pct = 100;
    }
    uint32_t duty = (uint32_t)(pct * ((1 << LED_DUTY_BITS) - 1) / 100.0f + 0.5f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

void led_set_rgb(float r, float g, float b)
{
    led_last_r = r;
    led_last_g = g;
    led_last_b = b;
    float k = led_levels[led_level];
    led_set(LED_RED, r * k);
    led_set(LED_GREEN, g * k);
    led_set(LED_BLUE, b * k);
}

void led_set_brightness(int level)
{
    if (level < 0)
    {
        level = 0;
    }
    if (level > LED_BRIGHTNESS_MAX)
    {
        level = LED_BRIGHTNESS_MAX;
    }
    led_level = level;
    /* apply to what is shown right now */
    led_set_rgb(led_last_r, led_last_g, led_last_b);
}

int led_get_brightness(void)
{
    return led_level;
}
