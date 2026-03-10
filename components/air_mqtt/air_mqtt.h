#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const uint8_t *mac;
    const char    *status;       // "online" / "offline"
    int            layer;
    const uint8_t *parent_mac;
    int            rssi;
    bool           authenticated;
} node_status_t;

esp_err_t mqtt_init(void);
void mqtt_publish_sensor(const uint8_t *mac, float temp, float smoke, float etx, uint8_t hops);
void mqtt_publish_node_status(const node_status_t *info);

void  send_mesh_config(uint8_t temp_max, uint8_t smoke_max);
