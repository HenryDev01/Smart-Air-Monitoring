#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t ble_bridge_init(void);
esp_err_t ble_advertise_sensor(float temp, float hum, float smoke);
void ble_node_start_advertising(void);
void ble_node_stop_advertising(void);
void ble_bridge_start_scanning(void);
void ble_bridge_stop_scanning(void);
bool ble_bridge_is_scanning(void);