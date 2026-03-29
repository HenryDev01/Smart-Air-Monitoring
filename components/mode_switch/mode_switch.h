#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ── Timing ──────────────────────────────────────────────── */
#define WIFI_MESH_JOIN_TIMEOUT_MS   30000
#define WIFI_MESH_CHECK_MS           2000
#define MODE_MONITOR_MS              15000
#define WIFI_LOSS_THRESHOLD              8   /* consecutive misses before demotion */
#define BLE_BEACON_HOLD_MS            8000   // 5100ms deinit + ~2000ms margin
#define BLE_ADV_JITTER_MAX_MS  5000   // max random delay before adv
#define WIFI_RSSI_DEMOTE_THRESHOLD  -75  // demote to BLE if signal drops below this


/* ── Mode enum ───────────────────────────────────────────── */
typedef enum {
    NODE_MODE_INIT,
    NODE_MODE_BLE_ONLY,     /* no wifi mesh — acting as BLE leaf node  */
    NODE_MODE_WIFI_BRIDGE,  /* in wifi mesh  — acting as BLE bridge    */
    NODE_MODE_BLE_STABILIZING,
} node_mode_t;

/* ── Public API ──────────────────────────────────────────── */

/**
 * @brief  Call once from app_main.
 *         Probes WiFi mesh, picks starting mode, spawns monitor task.
 */
void mode_init(void);

/**
 * @brief  Returns the current operating mode.
 */
node_mode_t mode_get(void);

/**
 * @brief  Try to join the WiFi mesh within timeout_ms.
 * @return true if mesh is alive before timeout expires.
 */
bool try_join_wifi_mesh(uint32_t timeout_ms);

/**
 * @brief  Tear down BLE node (if running), start WiFi mesh + BLE bridge.
 */
void enter_wifi_bridge_mode(void);

/**
 * @brief  Tear down WiFi mesh + BLE bridge (if running), start BLE node.
 */
void enter_ble_node_mode(void);

/**
 * @brief  Alias for enter_ble_node_mode — called when WiFi mesh is lost.
 */
void demote_to_ble_node_mode(void);

