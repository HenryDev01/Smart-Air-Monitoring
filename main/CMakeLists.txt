idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES mesh
    REQUIRES sensor
    REQUIRES display
    REQUIRES utils
    REQUIRES air_mqtt
    REQUIRES ble_mesh
)