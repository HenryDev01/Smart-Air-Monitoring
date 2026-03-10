idf_component_register(SRCS "sensor.c"
                       INCLUDE_DIRS "."
                       REQUIRES freertos esp_wifi nvs_flash utils)