idf_component_register(
    INCLUDE_DIRS "initialization" "routing" "flooding" "auth"
    REQUIRES freertos esp_wifi nvs_flash esp_timer mbedtls air_mqtt
    SRCS "initialization/mesh_init.c" "routing/mesh_routing.c" "flooding/mesh_gossip.c" "auth/mesh_auth.c" 
)
 