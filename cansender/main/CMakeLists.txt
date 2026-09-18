idf_component_register(SRCS "cansender.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_twai esp_driver_gpio)
