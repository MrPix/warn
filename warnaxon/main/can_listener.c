#include "can_listener.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"

#define CAN_TX_GPIO GPIO_NUM_21
#define CAN_RX_GPIO GPIO_NUM_22
#define CAN_RX_QUEUE_DEPTH 64
#define CAN_TIMESTAMP_HZ 1000000

const uint32_t can_bitrates_kbps[CAN_BITRATE_COUNT] = {10, 20, 50, 100, 125, 250, 500, 800, 1000};

int can_bitrate_index(uint32_t bitrate_kbps)
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

static bool IRAM_ATTR can_error_callback(twai_node_handle_t node, const twai_error_event_data_t *event, void *user_ctx)
{
	(void)node;
	(void)event;
	can_listener_t *listener = user_ctx;
	listener->bus_errors++;
	return false;
}

static bool IRAM_ATTR can_state_change_callback(twai_node_handle_t node, const twai_state_change_event_data_t *event, void *user_ctx)
{
	(void)node;
	(void)event;
	can_listener_t *listener = user_ctx;
	listener->state_changes++;
	return false;
}

esp_err_t can_listener_start(can_listener_t *listener, uint32_t bitrate_kbps, bool listen_only)
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
		.tx_queue_depth = listen_only ? 0 : 1,
		.flags.enable_listen_only = listen_only,
	};
	esp_err_t error = twai_new_node_onchip(&config, &listener->node);
	if (error != ESP_OK) {
		vQueueDelete(listener->rx_queue);
		listener->rx_queue = NULL;
		return error;
	}

	const twai_event_callbacks_t callbacks = {
		.on_rx_done = can_rx_callback,
		.on_error = can_error_callback,
		.on_state_change = can_state_change_callback,
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

esp_err_t can_transmitter_start(can_transmitter_t *transmitter, uint32_t bitrate_kbps)
{
	memset(transmitter, 0, sizeof(*transmitter));
	twai_onchip_node_config_t config = {
		.io_cfg = {
			.tx = CAN_TX_GPIO,
			.rx = CAN_RX_GPIO,
			.quanta_clk_out = GPIO_NUM_NC,
			.bus_off_indicator = GPIO_NUM_NC,
		},
		.bit_timing.bitrate = bitrate_kbps * 1000,
		.timestamp_resolution_hz = CAN_TIMESTAMP_HZ,
		.tx_queue_depth = 4,
	};
	esp_err_t error = twai_new_node_onchip(&config, &transmitter->node);
	if (error != ESP_OK) {
		return error;
	}
	error = twai_node_enable(transmitter->node);
	if (error != ESP_OK) {
		twai_node_delete(transmitter->node);
		transmitter->node = NULL;
		return error;
	}
	transmitter->bitrate_kbps = bitrate_kbps;
	return ESP_OK;
}

void can_transmitter_stop(can_transmitter_t *transmitter)
{
	if (transmitter->node != NULL) {
		twai_node_disable(transmitter->node);
		twai_node_delete(transmitter->node);
		transmitter->node = NULL;
	}
	transmitter->bitrate_kbps = 0;
}

esp_err_t can_transmitter_send(can_transmitter_t *transmitter, uint32_t id, const uint8_t *data, uint8_t data_len)
{
	twai_frame_t frame = {
		.header = {
			.id = id,
			.dlc = data_len,
		},
		.buffer = (uint8_t *)data,
		.buffer_len = data_len,
	};
	return twai_node_transmit(transmitter->node, &frame, 1000);
}

void can_listener_stop(can_listener_t *listener)
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

void can_print_frame(uint32_t bitrate_kbps, const can_rx_frame_t *frame, uint64_t *previous_timestamp)
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
