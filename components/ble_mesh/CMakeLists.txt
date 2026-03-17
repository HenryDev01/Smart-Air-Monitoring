idf_component_register(
    INCLUDE_DIRS "bridge"  "node"
    REQUIRES freertos esp_wifi nvs_flash esp_timer mbedtls bt esp_coex esp_event 
    SRCS "bridge/ble_bridge.c" "node/node.c"    
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC CONFIG_BLE_MESH_CONFIG_CLIENT_MODEL=1)
