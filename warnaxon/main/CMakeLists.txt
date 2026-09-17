idf_component_register(SRCS "console_setup.c" "cmd_can.c" "cmd_system.c" "cmd_led.c" "led_status.c" "can_listener.c" "warnaxon.c"
                    INCLUDE_DIRS "."
                    REQUIRES console esp_app_format esp_driver_gpio esp_driver_twai esp_system)
