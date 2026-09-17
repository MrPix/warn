#include <stdio.h>

#include "console_setup.h"
#include "led_status.h"
#include "sdkconfig.h"

void app_main(void)
{
	led_status_start();

	printf("\nWarnaxon CLI ready on UART%d at %d baud. Type 'help'.\n",
		CONFIG_ESP_CONSOLE_UART_NUM, CONFIG_ESP_CONSOLE_UART_BAUDRATE);
	console_start();
}
