idf_component_register(
    INCLUDE_DIRS "initialization" "routing" "flooding" "auth" "ble_mesh"
    REQUIRES freertos esp_wifi nvs_flash esp_timer mbedtls bt esp_coex
    SRCS
        "initialization/mesh_init.c"
        "routing/mesh_routing.c"
        "flooding/mesh_gossip.c"
        "auth/mesh_auth.c"
        "ble_mesh/ble_bridge.c"
)