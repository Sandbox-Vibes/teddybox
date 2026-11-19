
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/mcpwm.h"

#include "board.h"

static const char *TAG = "LED";

/* brightness levels 0..LED_BRIGHTNESS_MAX as share of the PWM duty; the eye
   is far from linear, equal steps would look almost the same at the top */
static const float led_levels[LED_BRIGHTNESS_MAX + 1] = {0.0f, 0.10f, 0.35f, 1.0f};
static int led_level = LED_BRIGHTNESS_MAX;
static float led_last_r, led_last_g, led_last_b;

void led_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, LED_RED_GPIO);
    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM1A, LED_GREEN_GPIO);
    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM2A, LED_BLUE_GPIO);

    mcpwm_config_t pwm_config = {
        .frequency = 1000,
        .cmpr_a = 0,
        .counter_mode = MCPWM_UP_COUNTER,
        .duty_mode = MCPWM_DUTY_MODE_0,
    };
    mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_config);
    mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_1, &pwm_config);
    mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_2, &pwm_config);

    led_set_rgb(0, 0, 0);
}

void led_set(led_t led, float pct)
{
    mcpwm_timer_t timer = MCPWM_TIMER_0;

    switch (led)
    {
    case LED_RED:
        timer = MCPWM_TIMER_0;
        break;

    case LED_GREEN:
        timer = MCPWM_TIMER_1;
        break;

    case LED_BLUE:
        timer = MCPWM_TIMER_2;
        break;
    }
    mcpwm_set_duty(MCPWM_UNIT_0, timer, MCPWM_GEN_A, pct);
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
