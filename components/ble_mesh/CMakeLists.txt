idf_component_register(
    INCLUDE_DIRS "bridge"  "node"
    REQUIRES freertos esp_wifi nvs_flash esp_timer mbedtls bt esp_coex esp_event driver sensor
    SRCS "bridge/ble_bridge.c" "node/node.c"    
)