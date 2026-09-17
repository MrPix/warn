#include "cmd_system.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_status.h"

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

void register_system_commands(void)
{
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

	const esp_console_cmd_t status_cmd = {
		.command = "status",
		.help = "Show device status summary",
		.hint = NULL,
		.func = &cmd_status,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));
}
