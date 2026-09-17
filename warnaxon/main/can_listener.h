#pragma once

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
} can_listener_t;

#define CAN_BITRATE_COUNT 9

extern const uint32_t can_bitrates_kbps[CAN_BITRATE_COUNT];

int can_bitrate_index(uint32_t bitrate_kbps);
esp_err_t can_listener_start(can_listener_t *listener, uint32_t bitrate_kbps);
void can_listener_stop(can_listener_t *listener);
void can_print_frame(uint32_t bitrate_kbps, const can_rx_frame_t *frame, uint64_t *previous_timestamp);
