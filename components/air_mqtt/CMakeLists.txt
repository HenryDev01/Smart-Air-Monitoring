idf_component_register(SRCS "air_mqtt.c"
                       INCLUDE_DIRS "."
                        REQUIRES mqtt freertos esp_wifi nvs_flash mbedtls json)