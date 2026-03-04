idf_component_register(
    INCLUDE_DIRS "initialization" "routing"
    REQUIRES freertos esp_wifi nvs_flash esp_timer
    SRCS "initialization/mesh_init.c" "routing/mesh_routing.c"
)
