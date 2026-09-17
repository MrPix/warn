#pragma once

#include <stdint.h>

typedef enum {
	LED_MODE_OFF,
	LED_MODE_ON,
	LED_MODE_BLINK,
} led_mode_t;

#define LED_BLINK_MIN_MS 50
#define LED_BLINK_MAX_MS 10000

void led_status_start(void);
void led_set_mode(led_mode_t mode, uint32_t blink_period_ms);
void led_get_state(led_mode_t *mode, uint32_t *blink_period_ms);
const char *led_mode_to_string(led_mode_t mode);
