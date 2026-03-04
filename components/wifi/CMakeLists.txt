idf_component_register(SRCS "wifi_service.c"
                       INCLUDE_DIRS "."
                       REQUIRES freertos esp_wifi nvs_flash)