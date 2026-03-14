idf_component_register(
    SRCS "DHT22.c"
    INCLUDE_DIRS "."
    REQUIRES driver freertos esp_common
)