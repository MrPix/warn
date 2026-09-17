#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"  // Selects stable vs advanced CLI line editing mode.
#include "sdkconfig.h"

#define LED_GPIO GPIO_NUM_2
#define LED_STATUS_TASK_STACK_SIZE 2048
#define LED_STATUS_TASK_PRIORITY 1
#define LED_BLINK_MIN_MS 50
#define LED_BLINK_MAX_MS 10000
#define CAN_TX_GPIO GPIO_NUM_21
#define CAN_RX_GPIO GPIO_NUM_22
#define CAN_RX_QUEUE_DEPTH 64
#define CAN_TIMESTAMP_HZ 1000000
#define CAN_SEARCH_DEFAULT_SECONDS 20
#define CAN_SEARCH_MAX_SECONDS 3600

static const uint32_t can_bitrates_kbps[] = {10, 20, 50, 100, 125, 250, 500, 800, 1000};
#define CAN_BITRATE_COUNT (sizeof(can_bitrates_kbps) / sizeof(can_bitrates_kbps[0]))

typedef struct {
	twai_frame_header_t header;
	uint8_t data[8];
	uint8_t data_len;
} can_rx_frame_t;

typedef struct {
	twai_node_handle_t node;
	QueueHandle_t rx_queue;
	volatile uint32_t dropped_frames;
} can_listener_t;

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

static int can_bitrate_index(uint32_t bitrate_kbps)
{
	for (size_t index = 0; index < CAN_BITRATE_COUNT; ++index) {
		if (can_bitrates_kbps[index] == bitrate_kbps) {
			return (int)index;
		}
	}
	return -1;
}

static bool IRAM_ATTR can_rx_callback(twai_node_handle_t node, const twai_rx_done_event_data_t *event, void *user_ctx)
{
	(void)event;
	can_listener_t *listener = user_ctx;
	can_rx_frame_t received = {0};
	twai_frame_t frame = {
		.buffer = received.data,
		.buffer_len = sizeof(received.data),
	};
	BaseType_t task_woken = pdFALSE;

	if (twai_node_receive_from_isr(node, &frame) != ESP_OK) {
		listener->dropped_frames++;
		return false;
	}

	received.header = frame.header;
	received.data_len = frame.header.rtr ? 0 : (frame.header.dlc > 8 ? 8 : frame.header.dlc);
	if (xQueueSendFromISR(listener->rx_queue, &received, &task_woken) != pdTRUE) {
		listener->dropped_frames++;
	}
	return task_woken == pdTRUE;
}

static esp_err_t can_listener_start(can_listener_t *listener, uint32_t bitrate_kbps)
{
	memset(listener, 0, sizeof(*listener));
	listener->rx_queue = xQueueCreate(CAN_RX_QUEUE_DEPTH, sizeof(can_rx_frame_t));
	if (listener->rx_queue == NULL) {
		return ESP_ERR_NO_MEM;
	}

	twai_onchip_node_config_t config = {
		.io_cfg = {
			.tx = CAN_TX_GPIO,
			.rx = CAN_RX_GPIO,
			.quanta_clk_out = GPIO_NUM_NC,
			.bus_off_indicator = GPIO_NUM_NC,
		},
		.bit_timing.bitrate = bitrate_kbps * 1000,
		.timestamp_resolution_hz = CAN_TIMESTAMP_HZ,
		.flags.enable_listen_only = true,
	};
	esp_err_t error = twai_new_node_onchip(&config, &listener->node);
	if (error != ESP_OK) {
		vQueueDelete(listener->rx_queue);
		listener->rx_queue = NULL;
		return error;
	}

	const twai_event_callbacks_t callbacks = {
		.on_rx_done = can_rx_callback,
	};
	error = twai_node_register_event_callbacks(listener->node, &callbacks, listener);
	if (error == ESP_OK) {
		error = twai_node_enable(listener->node);
	}
	if (error != ESP_OK) {
		twai_node_delete(listener->node);
		listener->node = NULL;
		vQueueDelete(listener->rx_queue);
		listener->rx_queue = NULL;
	}
	return error;
}

static void can_listener_stop(can_listener_t *listener)
{
	if (listener->node != NULL) {
		twai_node_disable(listener->node);
		twai_node_delete(listener->node);
		listener->node = NULL;
	}
	if (listener->rx_queue != NULL) {
		vQueueDelete(listener->rx_queue);
		listener->rx_queue = NULL;
	}
}

static void can_print_frame(uint32_t bitrate_kbps, const can_rx_frame_t *frame, uint64_t *previous_timestamp)
{
	printf("%" PRIu32 " kbps  %10" PRIu64 " us  delta=", bitrate_kbps, frame->header.timestamp);
	if (*previous_timestamp == 0) {
		printf("         -  ");
	} else {
		printf("%10" PRIu64 "  ", frame->header.timestamp - *previous_timestamp);
	}
	*previous_timestamp = frame->header.timestamp;

	if (frame->header.ide) {
		printf("EXT %08" PRIX32, frame->header.id);
	} else {
		printf("STD %03" PRIX32, frame->header.id);
	}
	printf("  [%u]%s", frame->header.dlc, frame->header.rtr ? " RTR" : "");
	for (uint8_t index = 0; index < frame->data_len; ++index) {
		printf(" %02X", frame->data[index]);
	}
	printf("\n");
}

static void can_print_usage(void)
{
	printf("Usage:\n");
	printf("  can search [--start <kbps>] [--end <kbps>] [--time <seconds>]\n");
	printf("  can sniff --bitrate <kbps>\n");
	printf("Bitrates: 10, 20, 50, 100, 125, 250, 500, 800, 1000 kbps\n");
}

static bool can_parse_option(int argc, char **argv, int *index, uint32_t min, uint32_t max, uint32_t *value)
{
	if (*index + 1 >= argc || !parse_u32_arg(argv[*index + 1], min, max, value)) {
		printf("Invalid or missing value for %s\n", argv[*index]);
		return false;
	}
	(*index)++;
	return true;
}

static int can_search(int argc, char **argv)
{
	uint32_t start_kbps = can_bitrates_kbps[0];
	uint32_t end_kbps = can_bitrates_kbps[CAN_BITRATE_COUNT - 1];
	uint32_t seconds = CAN_SEARCH_DEFAULT_SECONDS;
	bool have_start = false;
	bool have_end = false;
	bool have_time = false;

	for (int index = 2; index < argc; ++index) {
		if (strcmp(argv[index], "--start") == 0 && !have_start) {
			have_start = can_parse_option(argc, argv, &index, 1, 1000, &start_kbps);
			if (!have_start) return 1;
		} else if (strcmp(argv[index], "--end") == 0 && !have_end) {
			have_end = can_parse_option(argc, argv, &index, 1, 1000, &end_kbps);
			if (!have_end) return 1;
		} else if (strcmp(argv[index], "--time") == 0 && !have_time) {
			have_time = can_parse_option(argc, argv, &index, 1, CAN_SEARCH_MAX_SECONDS, &seconds);
			if (!have_time) return 1;
		} else {
			printf("Unknown or duplicate option: %s\n", argv[index]);
			can_print_usage();
			return 1;
		}
	}

	int start_index = can_bitrate_index(start_kbps);
	int end_index = can_bitrate_index(end_kbps);
	if (start_index < 0 || end_index < 0) {
		printf("Start and end must be supported standard bitrates.\n");
		can_print_usage();
		return 1;
	}

	bool found[CAN_BITRATE_COUNT] = {0};
	bool unsupported[CAN_BITRATE_COUNT] = {0};
	int direction = start_index <= end_index ? 1 : -1;
	for (int bitrate_index = start_index;; bitrate_index += direction) {
		uint32_t bitrate_kbps = can_bitrates_kbps[bitrate_index];
		if (bitrate_kbps == 10) {
			unsupported[bitrate_index] = true;
			printf("Skipping 10 kbps: bitrate is not achievable by the ESP32 TWAI clock.\n");
			if (bitrate_index == end_index) break;
			continue;
		}
		can_listener_t listener;
		printf("Searching at %" PRIu32 " kbps for %" PRIu32 " seconds...\n", bitrate_kbps, seconds);
		esp_err_t error = can_listener_start(&listener, bitrate_kbps);
		if (error != ESP_OK) {
			if (error == ESP_ERR_INVALID_ARG) {
				unsupported[bitrate_index] = true;
				printf("Skipping %" PRIu32 " kbps: bitrate is not achievable by this ESP32 TWAI clock.\n", bitrate_kbps);
				if (bitrate_index == end_index) break;
				continue;
			}
			printf("Cannot start %" PRIu32 " kbps listener: %s\n", bitrate_kbps, esp_err_to_name(error));
			return 1;
		}

		uint32_t frame_count = 0;
		uint64_t previous_timestamp = 0;
		TickType_t started_at = xTaskGetTickCount();
		TickType_t duration = pdMS_TO_TICKS(seconds * 1000);
		while ((xTaskGetTickCount() - started_at) < duration) {
			can_rx_frame_t frame;
			if (xQueueReceive(listener.rx_queue, &frame, pdMS_TO_TICKS(10)) == pdTRUE) {
				can_print_frame(bitrate_kbps, &frame, &previous_timestamp);
				frame_count++;
			}
		}
		found[bitrate_index] = frame_count > 0;
		printf("Result %" PRIu32 " kbps: %" PRIu32 " frames, %" PRIu32 " dropped%s\n",
			bitrate_kbps, frame_count, listener.dropped_frames, found[bitrate_index] ? ", traffic detected" : "");
		can_listener_stop(&listener);
		if (bitrate_index == end_index) break;
	}

	printf("Active bitrates:");
	bool any_found = false;
	for (size_t index = 0; index < CAN_BITRATE_COUNT; ++index) {
		if (found[index]) {
			printf(" %" PRIu32, can_bitrates_kbps[index]);
			any_found = true;
		}
	}
	printf(any_found ? " kbps\n" : " none\n");
	printf("Unsupported bitrates:");
	bool any_unsupported = false;
	for (size_t index = 0; index < CAN_BITRATE_COUNT; ++index) {
		if (unsupported[index]) {
			printf(" %" PRIu32, can_bitrates_kbps[index]);
			any_unsupported = true;
		}
	}
	printf(any_unsupported ? " kbps\n" : " none\n");
	return 0;
}

static int can_sniff(int argc, char **argv)
{
	uint32_t bitrate_kbps = 0;
	bool have_bitrate = false;
	for (int index = 2; index < argc; ++index) {
		if (strcmp(argv[index], "--bitrate") == 0 && !have_bitrate) {
			have_bitrate = can_parse_option(argc, argv, &index, 1, 1000, &bitrate_kbps);
			if (!have_bitrate) return 1;
		} else {
			printf("Unknown or duplicate option: %s\n", argv[index]);
			can_print_usage();
			return 1;
		}
	}
	if (!have_bitrate || can_bitrate_index(bitrate_kbps) < 0) {
		printf("--bitrate must be one of the supported standard bitrates.\n");
		can_print_usage();
		return 1;
	}
	if (bitrate_kbps == 10) {
		printf("Cannot sniff at 10 kbps: bitrate is not achievable by the ESP32 TWAI clock.\n");
		return 1;
	}

	can_listener_t listener;
	esp_err_t error = can_listener_start(&listener, bitrate_kbps);
	if (error != ESP_OK) {
		if (error == ESP_ERR_INVALID_ARG) {
			printf("Cannot sniff at %" PRIu32 " kbps: bitrate is not achievable by this ESP32 TWAI clock.\n", bitrate_kbps);
		} else {
			printf("Cannot start CAN listener: %s\n", esp_err_to_name(error));
		}
		return 1;
	}

	int stdin_fd = fileno(stdin);
	int original_flags = fcntl(stdin_fd, F_GETFL, 0);
	if (original_flags < 0 || fcntl(stdin_fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
		printf("Cannot enable nonblocking console input.\n");
		can_listener_stop(&listener);
		return 1;
	}
	printf("Sniffing at %" PRIu32 " kbps. Press lowercase 'c' to stop.\n", bitrate_kbps);
	uint32_t frame_count = 0;
	uint64_t previous_timestamp = 0;
	bool stop = false;
	while (!stop) {
		can_rx_frame_t frame;
		if (xQueueReceive(listener.rx_queue, &frame, pdMS_TO_TICKS(10)) == pdTRUE) {
			can_print_frame(bitrate_kbps, &frame, &previous_timestamp);
			frame_count++;
		}
		char input;
		while (read(stdin_fd, &input, 1) == 1) {
			if (input == 'c') {
				stop = true;
				break;
			}
		}
	}
	fcntl(stdin_fd, F_SETFL, original_flags);
	printf("Sniff stopped: %" PRIu32 " frames, %" PRIu32 " dropped\n", frame_count, listener.dropped_frames);
	can_listener_stop(&listener);
	return 0;
}

static int cmd_can(int argc, char **argv)
{
	if (argc < 2) {
		can_print_usage();
		return 1;
	}

	if (strcmp(argv[1], "search") == 0) {
		return can_search(argc, argv);
	}
	if (strcmp(argv[1], "sniff") == 0) {
		return can_sniff(argc, argv);
	}

	printf("Unknown CAN command: %s\n", argv[1]);
	can_print_usage();
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
		.help = "Search or sniff a classic CAN bus in listen-only mode",
		.hint = "search [--start kbps] [--end kbps] [--time seconds] | sniff --bitrate kbps",
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
	repl_config.max_cmdline_args = 9;
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
