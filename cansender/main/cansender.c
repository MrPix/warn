#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "driver/gpio.h"
#include "esp_log.h"

// --- User configuration ---
#define CAN_TX_GPIO         GPIO_NUM_21
#define CAN_RX_GPIO         GPIO_NUM_22
#define CAN_BITRATE         500000                          // change speed here (bits/s)
#define CAN_MSG_ID          0x123
#define CAN_MSG_DLC         8
#define CAN_SEND_INTERVAL_MS 100
#define LED_GPIO            GPIO_NUM_2
#define LED_BLINK_MS        20
// --- End configuration ---

static const char *TAG = "cansender";

void app_main(void)
{
    twai_node_handle_t node = NULL;
    twai_onchip_node_config_t node_config = {
        .io_cfg = {
            .tx = CAN_TX_GPIO,
            .rx = CAN_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = {
            .bitrate = CAN_BITRATE,
        },
        .tx_queue_depth = 5,
        .flags.enable_self_test = true, // allow transmit without another node acking (standalone testing)
    };

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &node));
    ESP_ERROR_CHECK(twai_node_enable(node));
    ESP_LOGI(TAG, "TWAI node started");

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    uint8_t data[CAN_MSG_DLC];
    twai_frame_t message = {
        .header.id = CAN_MSG_ID,
        .buffer = data,
        .buffer_len = sizeof(data),
    };

    uint32_t counter = 0;

    while (1) {
        for (int i = 0; i < CAN_MSG_DLC; i++) {
            data[i] = (uint8_t)(counter + i);
        }

        esp_err_t err = twai_node_transmit(node, &message, 1000);
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_MS));
        gpio_set_level(LED_GPIO, 0);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Sent frame id=0x%03lX counter=%lu", (unsigned long)message.header.id, (unsigned long)counter);
        } else {
            ESP_LOGW(TAG, "Failed to send frame: %s", esp_err_to_name(err));

            // Disabled: bus-off recovery was masking a hardware wiring issue
            // twai_node_status_t status;
            // twai_node_get_info(node, &status, NULL);
            // if (status.state == TWAI_ERROR_BUS_OFF) {
            //     ESP_LOGW(TAG, "Bus-off detected, recovering...");
            //     ESP_ERROR_CHECK(twai_node_recover(node));
            // }
        }

        counter++;
        vTaskDelay(pdMS_TO_TICKS(CAN_SEND_INTERVAL_MS));
    }
}
