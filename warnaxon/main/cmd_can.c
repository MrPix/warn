#include "cmd_can.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "can_listener.h"
#include "esp_console.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define CAN_SEARCH_DEFAULT_SECONDS 20
#define CAN_SEARCH_MAX_SECONDS 3600

static can_transmitter_t can_transmitter;

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

static void can_print_usage(void)
{
	printf("Usage:\n");
	printf("  can search [--start <kbps>] [--end <kbps>] [--time <seconds>]\n");
	printf("  can sniff --bitrate <kbps>\n");
	printf("  can setbitrate <kbps>\n");
	printf("  can send <id> [byte ...]\n");
	printf("Bitrates: 10, 20, 50, 100, 125, 250, 500, 800, 1000 kbps\n");
}

static bool parse_hex_u32_arg(const char *value, uint32_t max, uint32_t *result)
{
	char *end = NULL;
	errno = 0;
	unsigned long parsed = strtoul(value, &end, 16);
	if (errno != 0 || end == value || *end != '\0' || parsed > max) {
		return false;
	}
	*result = (uint32_t)parsed;
	return true;
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
	can_transmitter_stop(&can_transmitter);
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
		esp_err_t error = can_listener_start(&listener, bitrate_kbps, true);
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
		printf("Result %" PRIu32 " kbps: %" PRIu32 " frames, %" PRIu32 " dropped, %" PRIu32 " bus errors%s\n",
			bitrate_kbps, frame_count, listener.dropped_frames, listener.bus_errors, found[bitrate_index] ? ", traffic detected" : "");
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
	can_transmitter_stop(&can_transmitter);
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
	/* Sniff mode ACKs frames so a lone transmitter can complete transfers on a 2-node bus. */
	esp_err_t error = can_listener_start(&listener, bitrate_kbps, false);
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
	printf("Sniff stopped: %" PRIu32 " frames, %" PRIu32 " dropped, %" PRIu32 " bus errors\n", frame_count, listener.dropped_frames, listener.bus_errors);
	can_listener_stop(&listener);
	return 0;
}

static int can_setbitrate(int argc, char **argv)
{
	uint32_t bitrate_kbps;
	if (argc != 3 || !parse_u32_arg(argv[2], 1, 1000, &bitrate_kbps) || can_bitrate_index(bitrate_kbps) < 0) {
		printf("Usage: can setbitrate <kbps>\n");
		return 1;
	}
	if (bitrate_kbps == 10) {
		printf("Cannot set 10 kbps: bitrate is not achievable by the ESP32 TWAI clock.\n");
		return 1;
	}

	can_transmitter_stop(&can_transmitter);
	esp_err_t error = can_transmitter_start(&can_transmitter, bitrate_kbps);
	if (error != ESP_OK) {
		printf("Cannot set CAN bitrate to %" PRIu32 " kbps: %s\n", bitrate_kbps, esp_err_to_name(error));
		return 1;
	}
	printf("CAN transmit bitrate set to %" PRIu32 " kbps\n", bitrate_kbps);
	return 0;
}

static int can_send(int argc, char **argv)
{
	if (can_transmitter.node == NULL) {
		printf("Set a bitrate first with: can setbitrate <kbps>\n");
		return 1;
	}
	if (argc < 3 || argc > 11) {
		printf("Usage: can send <hex-id> [hex-byte ...]\n");
		return 1;
	}

	uint32_t id;
	if (!parse_hex_u32_arg(argv[2], TWAI_STD_ID_MASK, &id)) {
		printf("Invalid standard CAN ID: %s\n", argv[2]);
		return 1;
	}

	uint8_t data[TWAI_FRAME_MAX_LEN];
	uint8_t data_len = (uint8_t)(argc - 3);
	for (uint8_t index = 0; index < data_len; ++index) {
		uint32_t byte;
		if (!parse_hex_u32_arg(argv[index + 3], UINT8_MAX, &byte)) {
			printf("Invalid CAN data byte: %s\n", argv[index + 3]);
			return 1;
		}
		data[index] = (uint8_t)byte;
	}

	esp_err_t error = can_transmitter_send(&can_transmitter, id, data, data_len);
	if (error != ESP_OK) {
		printf("CAN transmit failed: %s\n", esp_err_to_name(error));
		return 1;
	}
	printf("Sent STD %03" PRIX32 " [%u]", id, data_len);
	for (uint8_t index = 0; index < data_len; ++index) {
		printf(" %02X", data[index]);
	}
	printf("\n");
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
	if (strcmp(argv[1], "setbitrate") == 0) {
		return can_setbitrate(argc, argv);
	}
	if (strcmp(argv[1], "send") == 0) {
		return can_send(argc, argv);
	}

	printf("Unknown CAN command: %s\n", argv[1]);
	can_print_usage();
	return 1;
}

void register_can_commands(void)
{
	const esp_console_cmd_t can_cmd = {
		.command = "can",
		.help = "Search, sniff, configure, or transmit classic CAN frames",
		.hint = "search [--start kbps] [--end kbps] [--time seconds] | sniff --bitrate kbps | setbitrate kbps | send hex-id [hex-byte ...]",
		.func = &cmd_can,
	};
	ESP_ERROR_CHECK(esp_console_cmd_register(&can_cmd));
}
