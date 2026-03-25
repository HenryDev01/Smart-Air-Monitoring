idf_component_register(
    SRCS "mode_switch.c"
    INCLUDE_DIRS "."
    REQUIRES mesh ble_mesh freertos esp_wifi nvs_flash esp_timer mbedtls sensor air_mqtt
  
)