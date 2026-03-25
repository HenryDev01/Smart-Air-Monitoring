#pragma once

/*
 * ble_bridge.h
 * ────────────
 * Adds BLE mesh listener to an existing ESP32 WiFi mesh node.
 * When a M5StickC sends OP_SENSOR_DATA via BLE mesh, this module
 * receives it and injects it into the WiFi mesh via esp_mesh_send().
 *
 * Uses the same vendor model + opcodes as m5stick_unified firmware
 * so M5StickC devices see every ESP32 node as a potential bridge.
 */

#include "esp_err.h"
#include <stdbool.h>

esp_err_t ble_bridge_init(void);
esp_err_t ble_bridge_deinit(void);
bool      ble_bridge_is_provisioned(void);
uint8_t   ble_bridge_get_load(void);   /* number of M5StickC using us */


//void ble_bridge_set_root(bool is_root);
