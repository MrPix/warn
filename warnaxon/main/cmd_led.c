#include "cmd_led.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_err.h"
#include "led_status.h"

static int cmd_led(int argc, char **argv)
{
	if (argc < 2) {
		printf("Usage: led on|off|blink <period_ms>\n");
		return 1;
	}

	if (strcmp(argv[1], "on") == 0) {
		led_set_mode(LED_MODE_ON, 0);
		printf("LED on\n");
		return 0;
	}

	if (strcmp(argv[1], "off") == 0) {
		led_set_mode(LED_MODE_OFF, 0);
		printf("LED off\n");
		return 0;
	}

	if (strcmp(argv[1], "blink") == 0) {
		led_mode_t mode;
		uint32_t period_ms;
		led_get_state(&mode, &period_ms);
		if (argc >= 3) {
			char *end = NULL;
			errno = 0;
			long parsed = strtol(argv[2], &end, 10);
			if (errno != 0 || end == argv[2] || *end != '\0' || parsed < LED_BLINK_MIN_MS || parsed > LED_BLINK_MAX_MS) {
				printf("Blink period must be %d..%d ms\n", LED_BLINK_MIN_MS, LED_BLINK_MAX_MS);
				return 1;
			}
			period_ms = (uint32_t)parsed;
		}

		led_set_mode(LED_MODE_BLINK, period_ms);
		printf("LED blinking every %" PRIu32 " ms\n", period_ms);
		return 0;
	}

	printf("Unknown LED command: %s\n", argv[1]);
	printf("Usage: led on|off|blink <period_ms>\n");
	return 1;
}

void register_led_commands(void)
{
	const esp_console_cmd_t led_cmd = {
		.command = "led",
		.help = "Control the GPIO2 status LED",
		.hint = "on|off|blink <period_ms>",
		.func = &cmd_led,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&led_cmd));
}
