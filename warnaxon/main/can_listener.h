#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
	twai_frame_header_t header;
	uint8_t data[8];
	uint8_t data_len;
} can_rx_frame_t;

typedef struct {
	twai_node_handle_t node;
	QueueHandle_t rx_queue;
	volatile uint32_t dropped_frames;
	volatile uint32_t bus_errors;
	volatile uint32_t state_changes;
} can_listener_t;

typedef struct {
	twai_node_handle_t node;
	uint32_t bitrate_kbps;
} can_transmitter_t;

#define CAN_BITRATE_COUNT 9

extern const uint32_t can_bitrates_kbps[CAN_BITRATE_COUNT];

int can_bitrate_index(uint32_t bitrate_kbps);
esp_err_t can_listener_start(can_listener_t *listener, uint32_t bitrate_kbps, bool listen_only);
void can_listener_stop(can_listener_t *listener);
esp_err_t can_transmitter_start(can_transmitter_t *transmitter, uint32_t bitrate_kbps);
void can_transmitter_stop(can_transmitter_t *transmitter);
esp_err_t can_transmitter_send(can_transmitter_t *transmitter, uint32_t id, const uint8_t *data, uint8_t data_len);
void can_print_frame(uint32_t bitrate_kbps, const can_rx_frame_t *frame, uint64_t *previous_timestamp);
