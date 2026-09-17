#include "led_status.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_GPIO GPIO_NUM_2
#define LED_STATUS_TASK_STACK_SIZE 2048
#define LED_STATUS_TASK_PRIORITY 1

static portMUX_TYPE led_state_lock = portMUX_INITIALIZER_UNLOCKED;
static led_mode_t led_mode = LED_MODE_BLINK;
static uint32_t led_blink_period_ms = 1000;

const char *led_mode_to_string(led_mode_t mode)
{
	switch (mode) {
	case LED_MODE_OFF:
		return "off";
	case LED_MODE_ON:
		return "on";
	case LED_MODE_BLINK:
		return "blink";
	default:
		return "unknown";
	}
}

void led_set_mode(led_mode_t mode, uint32_t blink_period_ms)
{
	portENTER_CRITICAL(&led_state_lock);
	led_mode = mode;
	if (blink_period_ms > 0) {
		led_blink_period_ms = blink_period_ms;
	}
	portEXIT_CRITICAL(&led_state_lock);

	if (mode == LED_MODE_ON) {
		gpio_set_level(LED_GPIO, 1);
	} else if (mode == LED_MODE_OFF) {
		gpio_set_level(LED_GPIO, 0);
	}
}

void led_get_state(led_mode_t *mode, uint32_t *blink_period_ms)
{
	portENTER_CRITICAL(&led_state_lock);
	*mode = led_mode;
	*blink_period_ms = led_blink_period_ms;
	portEXIT_CRITICAL(&led_state_lock);
}

static void led_status_task(void *arg)
{
	(void)arg;
	bool led_level = false;

	while (true) {
		led_mode_t mode;
		uint32_t blink_period_ms;
		led_get_state(&mode, &blink_period_ms);

		if (mode == LED_MODE_BLINK) {
			led_level = !led_level;
			gpio_set_level(LED_GPIO, led_level);
			vTaskDelay(pdMS_TO_TICKS(blink_period_ms / 2));
		} else {
			led_level = (mode == LED_MODE_ON);
			vTaskDelay(pdMS_TO_TICKS(250));
		}
	}
}

void led_status_start(void)
{
	ESP_ERROR_CHECK(gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT));
	led_set_mode(LED_MODE_BLINK, 0);
	ESP_ERROR_CHECK(xTaskCreate(led_status_task, "led_status", LED_STATUS_TASK_STACK_SIZE, NULL,
		LED_STATUS_TASK_PRIORITY, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
}
