#pragma once

#include "esp_err.h"
#include "esp_ble_mesh_defs.h"
#include "../../configuration/air_mesh.h"

/* ─────────────────────────────────────────────
   BLE MESH ROLES
   ───────────────────────────────────────────── */
typedef enum {
    BLE_MESH_ROLE_NODE        = 0,   // regular sensor node
    BLE_MESH_ROLE_PROVISIONER = 1,   // root node provisions others
} ble_mesh_role_t;

/* ─────────────────────────────────────────────
   PUBLIC API
   ───────────────────────────────────────────── */

// Call from main.c after mesh_init()
esp_err_t ble_mesh_init(void);

// Send sensor data over BLE mesh (called from sensor.c)
esp_err_t ble_mesh_send_sensor(float temperature, float humidity, float smoke);

// Send alert/gossip over BLE mesh
esp_err_t ble_mesh_send_alert(const char *msg, uint8_t len);

// Send HELLO over BLE mesh
esp_err_t ble_mesh_send_hello(float etx_to_root, uint8_t hop_count);

// Get current BLE mesh role
ble_mesh_role_t ble_mesh_get_role(void);

// Check if BLE mesh is provisioned and ready
bool ble_mesh_is_ready(void);