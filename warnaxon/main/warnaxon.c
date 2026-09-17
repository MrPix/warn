#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"  // Selects stable vs advanced CLI line editing mode.
#include "sdkconfig.h"

#define LED_GPIO GPIO_NUM_2
#define LED_STATUS_TASK_STACK_SIZE 2048
#define LED_STATUS_TASK_PRIORITY 1
#define LED_BLINK_MIN_MS 50
#define LED_BLINK_MAX_MS 10000
#define CAN_SNIFF_DEFAULT_COUNT 20
#define CAN_SNIFF_MAX_COUNT 1000
#define CAN_SNIFF_PERIOD_MS 500
#define CAN_SNIFF_MIN_PERIOD_MS 10
#define CAN_SNIFF_MAX_PERIOD_MS 10000

typedef enum {
	LED_MODE_OFF,
	LED_MODE_ON,
	LED_MODE_BLINK,
} led_mode_t;

static portMUX_TYPE led_state_lock = portMUX_INITIALIZER_UNLOCKED;
static led_mode_t led_mode = LED_MODE_BLINK;
static uint32_t led_blink_period_ms = 1000;

static const char *led_mode_to_string(led_mode_t mode)
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

static void led_set_mode(led_mode_t mode, uint32_t blink_period_ms)
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

static void led_get_state(led_mode_t *mode, uint32_t *blink_period_ms)
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

static int cmd_heap(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("Free heap: %" PRIu32 " bytes\n", esp_get_free_heap_size());
	printf("Minimum free heap: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());
	return 0;
}

static int cmd_version(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	const esp_app_desc_t *app = esp_app_get_description();
	printf("Project: %s\n", app->project_name);
	printf("App version: %s\n", app->version);
	printf("Built: %s %s\n", app->date, app->time);
	printf("ESP-IDF: %s\n", app->idf_ver);
	return 0;
}

static int cmd_restart(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("Restarting...\n");
	fflush(stdout);
	vTaskDelay(pdMS_TO_TICKS(100));
	esp_restart();
}

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

static int cmd_status(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	led_mode_t mode;
	uint32_t blink_period_ms;
	led_get_state(&mode, &blink_period_ms);

	printf("LED: %s", led_mode_to_string(mode));
	if (mode == LED_MODE_BLINK) {
		printf(" (%" PRIu32 " ms)", blink_period_ms);
	}
	printf("\n");
	printf("Free heap: %" PRIu32 " bytes\n", esp_get_free_heap_size());
	printf("Reset reason: %d\n", esp_reset_reason());
	return 0;
}

static bool parse_u32_arg(const char *value, uint32_t min, uint32_t max, uint32_t *result)
{
	char *end = NULL;
	errno = 0;
	long parsed = strtol(value, &end, 10);

	if (errno != 0 || end == value || *end != '\0' || parsed < (long)min || parsed > (long)max) {
		return false;
	}

	*result = (uint32_t)parsed;
	return true;
}

static int cmd_can(int argc, char **argv)
{
	if (argc < 2) {
		printf("Usage: can sniff [count] [period_ms]\n");
		return 1;
	}

	if (strcmp(argv[1], "sniff") == 0) {
		uint32_t count = CAN_SNIFF_DEFAULT_COUNT;
		uint32_t period_ms = CAN_SNIFF_PERIOD_MS;

		if (argc >= 3 && !parse_u32_arg(argv[2], 1, CAN_SNIFF_MAX_COUNT, &count)) {
			printf("Count must be 1..%d\n", CAN_SNIFF_MAX_COUNT);
			return 1;
		}

		if (argc >= 4 && !parse_u32_arg(argv[3], CAN_SNIFF_MIN_PERIOD_MS, CAN_SNIFF_MAX_PERIOD_MS, &period_ms)) {
			printf("Period must be %d..%d ms\n", CAN_SNIFF_MIN_PERIOD_MS, CAN_SNIFF_MAX_PERIOD_MS);
			return 1;
		}

		for (uint32_t frame = 0; frame < count; ++frame) {
			printf("8  01 02 03 04 05 06 07 08\n");
			if (frame + 1 < count) {
				vTaskDelay(pdMS_TO_TICKS(period_ms));
			}
		}
		return 0;
	}

	if (strcmp(argv[1], "stop") == 0) {
		printf("CAN sniff uses bounded output now; no background stream is running.\n");
		return 0;
	}

	if (strcmp(argv[1], "status") == 0) {
		printf("CAN sniff: stopped\n");
		return 0;
	}

	printf("Unknown CAN command: %s\n", argv[1]);
	printf("Usage: can sniff [count] [period_ms]\n");
	return 1;
}

static void register_console_commands(void)
{
	ESP_ERROR_CHECK(esp_console_register_help_command());

	const esp_console_cmd_t heap_cmd = {
		.command = "heap",
		.help = "Show free heap information",
		.hint = NULL,
		.func = &cmd_heap,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&heap_cmd));

	const esp_console_cmd_t version_cmd = {
		.command = "version",
		.help = "Show firmware and ESP-IDF version information",
		.hint = NULL,
		.func = &cmd_version,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&version_cmd));

	const esp_console_cmd_t restart_cmd = {
		.command = "restart",
		.help = "Restart the device",
		.hint = NULL,
		.func = &cmd_restart,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&restart_cmd));

	const esp_console_cmd_t led_cmd = {
		.command = "led",
		.help = "Control the GPIO2 status LED",
		.hint = "on|off|blink <period_ms>",
		.func = &cmd_led,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&led_cmd));

	const esp_console_cmd_t status_cmd = {
		.command = "status",
		.help = "Show device status summary",
		.hint = NULL,
		.func = &cmd_status,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

	const esp_console_cmd_t can_cmd = {
		.command = "can",
		.help = "Dummy CAN tools",
		.hint = "sniff [count] [period_ms]",
		.func = &cmd_can,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&can_cmd));
}

static void start_console(void)
{
	esp_console_repl_t *repl = NULL;
	esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
	repl_config.prompt = "warnaxon> ";
	repl_config.max_cmdline_length = 128;
	repl_config.max_cmdline_args = 8;
	ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));

	/*
	 * Stable serial terminal mode:
	 * ESP-IDF's console REPL uses linenoise for advanced line editing,
	 * command history, hints, and TAB completion. That mode redraws the
	 * current input line with ANSI cursor-control sequences on every keypress.
	 *
	 * Some PC serial terminals show that redraw as the cursor jumping back to
	 * the start of the line while typing. Disabling multiline + dumb mode keeps
	 * input visually stable, but reduces advanced editing features.
	 *
	 * To switch back to the advanced CLI behavior, remove these two calls:
	 *     linenoiseSetMultiLine(0);
	 *     linenoiseSetDumbMode(1);
	 */
	linenoiseSetMultiLine(0);
	linenoiseSetDumbMode(1);
	register_console_commands();
	ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void)
{
	ESP_ERROR_CHECK(gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT));
	led_set_mode(LED_MODE_BLINK, led_blink_period_ms);
	ESP_ERROR_CHECK(xTaskCreate(led_status_task, "led_status", LED_STATUS_TASK_STACK_SIZE, NULL,
		LED_STATUS_TASK_PRIORITY, NULL) == pdPASS ? ESP_OK : ESP_FAIL);

	printf("\nWarnaxon CLI ready on UART%d at %d baud. Type 'help'.\n",
		CONFIG_ESP_CONSOLE_UART_NUM, CONFIG_ESP_CONSOLE_UART_BAUDRATE);
	start_console();
}
