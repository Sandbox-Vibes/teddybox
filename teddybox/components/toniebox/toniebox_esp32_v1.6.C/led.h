#pragma once

typedef enum
{
    LED_RED,
    LED_GREEN,
    LED_BLUE,
} led_t;

void led_init(void);
void led_set(led_t led, float pct);
void led_set_rgb(float r, float g, float b);

/* 0 = off .. LED_BRIGHTNESS_MAX = full, scales every colour that is set */
#define LED_BRIGHTNESS_MAX 3
void led_set_brightness(int level);
int led_get_brightness(void);
