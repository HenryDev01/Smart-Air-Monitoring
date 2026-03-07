idf_component_register(SRCS "sensor.c" "dht22/DHT22.c"
                       INCLUDE_DIRS "." "dht22"
                       REQUIRES freertos esp_wifi nvs_flash driver esp_common display)