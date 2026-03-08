idf_component_register(
    INCLUDE_DIRS "initialization" "routing" "ble_mesh"
    REQUIRES freertos esp_wifi nvs_flash esp_timer bt esp_coex display
    SRCS "initialization/mesh_init.c" "routing/mesh_routing.c" "ble_mesh/ble_mesh_init.c"
)
