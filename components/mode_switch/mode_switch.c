#include "mode_switch.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_wifi.h"
#include "esp_random.h"   // ← for esp_random() jitter
#include "nvs_flash.h"

#include "../mesh/initialization/mesh_init.h"
#include "../mesh/routing/mesh_routing.h"
#include "../sensor/sensor.h"
#include "../mesh/flooding/mesh_gossip.h"
#include "../mesh/auth/mesh_auth.h"
#include "../ble_mesh/bridge/ble_bridge.h"
#include "../ble_mesh/node/node.h"
#include "../air_mqtt/air_mqtt.h"

static const char *TAG = "MODE_SWITCH";

/* ── Internal state ──────────────────────────────────────── */
static node_mode_t       s_mode              = NODE_MODE_INIT;
static bool              s_ble_node_running  = false;
static bool              s_wifi_running      = false;
static bool              s_ble_bridge_running = false;
static bool              s_wifi_modules_init  = false;
static SemaphoreHandle_t s_mode_mutex        = NULL;
static volatile bool s_wifi_probe_active = false;


/* ═══════════════════════════════════════════════════════════
   INTERNAL HELPERS
   ═══════════════════════════════════════════════════════════ */
static bool wifi_mesh_alive(void)
{
    return mesh_is_healthy();
}

/* ═══════════════════════════════════════════════════════════
   PUBLIC: try_join_wifi_mesh
   ═══════════════════════════════════════════════════════════ */
bool try_join_wifi_mesh(uint32_t timeout_ms)
{
    // --- Guard against concurrent probes ---
    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    if (s_wifi_probe_active) {
        xSemaphoreGive(s_mode_mutex);
        ESP_LOGI(TAG, "probe already in progress, skipping");
        return false;
    }
    s_wifi_probe_active = true;

    // Snapshot mode so we know whether to suppress beacon
    bool was_ble_only = (s_ble_node_running && s_mode == NODE_MODE_BLE_ONLY);

    // if (was_ble_only) {
    //     ESP_LOGI(TAG, "  suspending beacon before WiFi probe...");
    //     esp_ble_mesh_node_prov_disable(
    //         ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
    // }

    if (!s_wifi_modules_init) {
        auth_init();
        routing_init();
        gossip_init();
        sensor_init();
        s_wifi_modules_init = true;
    }

    esp_err_t err = mesh_init();
    xSemaphoreGive(s_mode_mutex);  // ← release BEFORE the wait loop

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mesh_init: %s", esp_err_to_name(err));
        xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
        s_wifi_probe_active = false;
        xSemaphoreGive(s_mode_mutex);
        return false;
    }

    // --- Wait loop: NO mutex held ---
    bool joined = false;
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (wifi_mesh_alive()) {
            ESP_LOGI(TAG, "Joined WiFi mesh after %"PRIu32" ms", elapsed);
            joined = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_MESH_CHECK_MS));
        elapsed += WIFI_MESH_CHECK_MS;
        ESP_LOGI(TAG, "  waiting for wifi mesh... %"PRIu32"/%"PRIu32" ms",
                 elapsed, timeout_ms);
    }

    // --- Teardown: re-acquire mutex only for shared state ---
    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);

    if (!joined) {
        ESP_LOGI(TAG, "Before mesh_deinit");
        mesh_deinit();
        auth_deinit();
        routing_deinit();
        gossip_deinit();
        sensor_deinit();
        s_wifi_modules_init = false;

        ESP_LOGI(TAG, "Before esp_mesh_stop");
        esp_mesh_stop();
        ESP_LOGI(TAG, "After esp_mesh_stop");

        // Re-check current mode — it may have changed during the wait
        if (s_ble_node_running && s_mode == NODE_MODE_BLE_ONLY) {
            ESP_LOGI(TAG, "  re-enabling beacon after failed probe");
            esp_ble_mesh_node_prov_enable(
                ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        }
    }

    s_wifi_probe_active = false;
    xSemaphoreGive(s_mode_mutex);
    return joined;
}
/* ═══════════════════════════════════════════════════════════
   INTERNAL: ble_stabilize_and_advertise_task
   Runs as a short-lived task so enter_ble_node_mode() can
   return without blocking the caller for the full hold-off
   + jitter window.
   ═══════════════════════════════════════════════════════════ */
static void ble_stabilize_and_advertise_task(void *arg)
{
    /* ── Option 3: Stabilization hold-off ──────────────────
       Stay in NODE_MODE_BLE_STABILIZING for BLE_BEACON_HOLD_MS.
       During this window we are invisible to provisioners.   */
    ESP_LOGI(TAG, "[stabilize] hold-off %d ms before advertising...",
             BLE_BEACON_HOLD_MS);
    vTaskDelay(pdMS_TO_TICKS(BLE_BEACON_HOLD_MS));

    // /* Bonus: probe WiFi one final time while stabilising.
    //    If the mesh recovered we jump straight to bridge mode
    //    and never advertise BLE at all.                        */
    // ESP_LOGI(TAG, "[stabilize] final WiFi probe during stabilization...");
    // bool recovered = try_join_wifi_mesh(WIFI_MESH_JOIN_TIMEOUT_MS);
    // if (recovered) {
    //     ESP_LOGI(TAG, "[stabilize] WiFi recovered — entering bridge mode");
    //     enter_wifi_bridge_mode();
    //     vTaskDelete(NULL);
    //     return;
    // }

    /* ── Option 1: Random jitter ────────────────────────────
       Each node picks a different random delay (0–5 s) before
       enabling BLE advertising.  This prevents two nodes that
       demoted at the same time from both advertising at the
       exact same instant and provisioning each other.        */
    uint32_t jitter_ms = (uint32_t)(esp_random() % (BLE_ADV_JITTER_MAX_MS + 1));
    ESP_LOGI(TAG, "[stabilize] jitter delay: %"PRIu32" ms", jitter_ms);
    vTaskDelay(pdMS_TO_TICKS(jitter_ms));

    /* Now it is safe to start advertising. */
    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    if (s_mode == NODE_MODE_BLE_STABILIZING) {          // guard: mode may have
        s_mode = NODE_MODE_BLE_ONLY;                    // changed during wait
        xSemaphoreGive(s_mode_mutex);

        esp_ble_mesh_node_prov_enable(
            ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        ESP_LOGI(TAG, "[stabilize] BLE node fully active — now provisionable");
    } else {
        xSemaphoreGive(s_mode_mutex);
        ESP_LOGI(TAG, "[stabilize] mode changed during stabilization (%d), skipping adv enable",
                 s_mode);
    }

    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════
   PUBLIC: enter_ble_node_mode
   ═══════════════════════════════════════════════════════════ */
void enter_ble_node_mode(void)
{
    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "→ Entering BLE NODE mode (stabilizing first)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_err_t err = node_init_silent();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "node_init failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mode_mutex);
        return;
    }

    s_ble_node_running = true;

    /* ── Option 3: Start in STABILIZING, not BLE_ONLY ──────
       The node is NOT yet provisionable.  The background task
       will flip to BLE_ONLY after hold-off + jitter.         */
    s_mode = NODE_MODE_BLE_STABILIZING;
    xSemaphoreGive(s_mode_mutex);

    /* DO NOT call esp_ble_mesh_node_prov_enable() here.
       The stabilize task does it after the safety window.    */

    xTaskCreatePinnedToCore(
        ble_stabilize_and_advertise_task,
        "ble_stabilize",
        4096, NULL,
        tskIDLE_PRIORITY + 1,   // lower than monitor so it doesn't starve it
        NULL, 0
    );

    ESP_LOGI(TAG, "BLE stabilization task started — monitor will probe WiFi every %d ms",
             MODE_MONITOR_MS);
}

/* ═══════════════════════════════════════════════════════════
   PUBLIC: enter_wifi_bridge_mode
   ═══════════════════════════════════════════════════════════ */
void enter_wifi_bridge_mode(void)
{
    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "→ Entering WIFI BRIDGE mode");

    if (s_ble_node_running) {
        ESP_LOGI(TAG, "  stopping BLE node...");
        node_deinit();
        s_ble_node_running = false;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_wifi_running = true;

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_err_t err = ble_bridge_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ble_bridge_init failed: %s", esp_err_to_name(err));
    } else {
        s_ble_bridge_running = true;
        ESP_LOGI(TAG, "BLE bridge active");
    }

    s_mode = NODE_MODE_WIFI_BRIDGE;
    ESP_LOGI(TAG, "WiFi bridge mode active");

    xSemaphoreGive(s_mode_mutex);
}

/* ═══════════════════════════════════════════════════════════
   PUBLIC: demote_to_ble_node_mode
   ═══════════════════════════════════════════════════════════ */
void demote_to_ble_node_mode(void)
{
    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);

    ESP_LOGW(TAG, "→ Demoting to BLE NODE mode (WiFi mesh lost)");

    if (s_ble_bridge_running) {
        ESP_LOGI(TAG, "  stopping BLE bridge...");
        ble_bridge_deinit();
        s_ble_bridge_running = false;
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    if (s_wifi_running) {
        ESP_LOGI(TAG, "  stopping WiFi mesh...");
        mesh_deinit();
        esp_mesh_stop();
        mqtt_deinit();
        esp_wifi_stop();
        auth_deinit();
        routing_deinit();
        gossip_deinit();
        sensor_deinit();
        s_wifi_modules_init = false;
        s_wifi_running = false;
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    xSemaphoreGive(s_mode_mutex);  // unlock BEFORE enter_ble_node_mode
                                    // because enter_ble_node_mode also takes mutex
    enter_ble_node_mode();
}

/* ═══════════════════════════════════════════════════════════
   INTERNAL: mode_monitor_task
   ═══════════════════════════════════════════════════════════ */
static void mode_monitor_task(void *arg)
{
    uint8_t wifi_loss_count = 0;
    uint8_t ble_retry_count = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MODE_MONITOR_MS));

        switch (s_mode) {

        /* ── Option 3: Guard — do nothing while stabilizing ──
           The stabilize task is handling things.  The monitor
           must not try to probe WiFi or call enter_wifi_bridge
           during this window, as it would race the task.      */
        case NODE_MODE_BLE_STABILIZING: {
            ESP_LOGI(TAG, "[monitor] stabilizing — probing WiFi opportunistically...");
            bool joined = try_join_wifi_mesh(WIFI_MESH_JOIN_TIMEOUT_MS);
            if (joined) {
                enter_wifi_bridge_mode(); // this sets s_mode, stabilize task will see the guard
            }
            break;
            break;
        }

        case NODE_MODE_BLE_ONLY: {
            if (ble_retry_count > 0) {
                ble_retry_count--;
                ESP_LOGI(TAG, "[monitor] BLE-only — backoff, skipping probe (%d ticks left)",
                         ble_retry_count);
                break;
            }

            ESP_LOGI(TAG, "[monitor] BLE-only — probing WiFi mesh...");
            bool joined = try_join_wifi_mesh(WIFI_MESH_JOIN_TIMEOUT_MS);
            if (joined) {
                enter_wifi_bridge_mode();
                wifi_loss_count = 0;
                ble_retry_count = 0;
            } else {
                ble_retry_count = (ble_retry_count == 0) ? 1 :
                                  (ble_retry_count >= 10) ? 10 :
                                  ble_retry_count * 2;
                ESP_LOGI(TAG, "[monitor] No WiFi mesh found, backing off %d ticks (~%d ms)",
                         ble_retry_count, ble_retry_count * MODE_MONITOR_MS);
            }
            break;
        }

        case NODE_MODE_WIFI_BRIDGE: {
            ble_retry_count = 0;
            if (!wifi_mesh_alive()) {
                wifi_loss_count++;
                ESP_LOGW(TAG, "[monitor] WiFi mesh not alive (%d/%d)",
                         wifi_loss_count, WIFI_LOSS_THRESHOLD);
                // if (wifi_loss_count == 2) {
                //     mesh_deinit();
                //     esp_mesh_stop();
                //     esp_ble_mesh_node_prov_disable(
                //         ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
                // }

                if (wifi_loss_count >= WIFI_LOSS_THRESHOLD) {
                    wifi_loss_count = 0;
                    demote_to_ble_node_mode();
                }
            } else {
                if (wifi_loss_count > 0)
                    ESP_LOGI(TAG, "[monitor] WiFi mesh recovered");
                wifi_loss_count = 0;
                ESP_LOGI(TAG, "[monitor] WiFi mesh OK | BLE bridge load: %d",
                         ble_bridge_get_load());
            }
            break;
        }

        default:
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════
   PUBLIC: mode_init
   ═══════════════════════════════════════════════════════════ */
void mode_init(void)
{
    s_mode_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mode_mutex);

    ESP_LOGI(TAG, "Boot: probing WiFi mesh for %d ms...",
             WIFI_MESH_JOIN_TIMEOUT_MS);

    bool joined = try_join_wifi_mesh(WIFI_MESH_JOIN_TIMEOUT_MS);

    ESP_LOGI(TAG, "Joined %d", joined);
    if (joined) {
        enter_wifi_bridge_mode();
    } else {
        ESP_LOGW(TAG, "No WiFi mesh at boot — starting as BLE node");
        enter_ble_node_mode();
    }

    xTaskCreatePinnedToCore(
        mode_monitor_task, "mode_monitor",
        4096, NULL,
        tskIDLE_PRIORITY + 2,
        NULL, 0
    );

    ESP_LOGI(TAG, "Mode monitor started");
}

/* ═══════════════════════════════════════════════════════════
   PUBLIC: mode_get
   ═══════════════════════════════════════════════════════════ */
node_mode_t mode_get(void)
{
    return s_mode;
}