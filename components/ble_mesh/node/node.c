/*
 * m5stick_node.c
 * ───────────────
 * M5StickC Plus 1.1 — BLE Mesh sensor node
 *
 * - Boots → broadcasts unprovisioned beacon
 * - Bridge (provisioner) sees it → provisions it automatically
 * - After provisioned: reads sensors, sends OP_SENSOR_DATA to bridge
 * - relay = ENABLED so it can relay for other nodes further out
 */

#include "node.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../../configuration/air_ble_mesh.h"

static const char *TAG = "M5_NODE";

#define SENSOR_PUBLISH_MS  15000
#define SENSOR_TASK_STACK  4096



/* ── State ───────────────────────────────────────────────── */
static uint8_t  s_dev_uuid[16] = {0};
static uint8_t  s_my_mac[6]    = {0};
static bool     s_provisioned  = false;
static uint16_t s_net_idx      = 0;
static uint16_t s_app_idx      = 0xFFFF;
static uint16_t s_my_addr      = 0;

/* ── Prov struct ─────────────────────────────────────────── */
static esp_ble_mesh_prov_t s_prov = {
    .uuid        = s_dev_uuid,
    .output_size = 0,
    .input_size  = 0,
};

/* ── Config server ───────────────────────────────────────── */
static esp_ble_mesh_cfg_srv_t s_cfg_srv = {
    .relay            = ESP_BLE_MESH_RELAY_ENABLED,
    .beacon           = ESP_BLE_MESH_BEACON_ENABLED,
    .friend_state     = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .gatt_proxy       = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
    .default_ttl      = 7,
    .net_transmit     = ESP_BLE_MESH_TRANSMIT(3, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(3, 20),
};

/* ── Vendor model ────────────────────────────────────────── */
static esp_ble_mesh_model_op_t s_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(OP_SENSOR_ACK,    1),
    ESP_BLE_MESH_MODEL_OP(OP_BRIDGE_ADVERT, sizeof(ble_bridge_advert_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(s_vnd_pub,
    2 + sizeof(ble_sensor_payload_t), ROLE_NODE);

static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_cfg_srv),
};
static esp_ble_mesh_model_t s_vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, VENDOR_MODEL_ID,
                              s_vnd_ops, &s_vnd_pub, NULL),
};
static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, s_vnd_models),
};
static esp_ble_mesh_comp_t s_comp = {
    .cid = CID_ESP, .pid = 0x0001, .vid = 0x0001,
    .element_count = ARRAY_SIZE(s_elements),
    .elements      = s_elements,
};

// task
static TaskHandle_t s_sensor_handle = NULL;


static bool s_config_complete = false;

bool node_is_config_complete(void) {
    return s_config_complete;
}

/* ═══════════════════════════════════════════════════════════
   READ SENSORS  — replace stubs with real drivers
   ═══════════════════════════════════════════════════════════ */
static void read_sensors(ble_sensor_payload_t *out)
{
    memcpy(out->src_mac, s_my_mac, 6);
    out->temperature = 28.5f;   /* TODO: SHT31 */
    out->smoke       = 12.0f;   /* TODO: MQ-2  */
    out->battery_pct = 85;      /* TODO: ADC   */
}

/* ═══════════════════════════════════════════════════════════
   PUBLISH SENSOR DATA → BRIDGE
   Sends unicast directly to bridge (GROUP_SENSOR 0xC000).
   Bridge receives it in model_cb and forwards to WiFi mesh.
   ═══════════════════════════════════════════════════════════ */
static void publish_sensor(void)
{
    if (!s_provisioned || s_app_idx == 0xFFFF) {
        ESP_LOGI(TAG, "Not provisioned yet — waiting for bridge...");
        return;
    }

    ble_sensor_payload_t payload = {0};
    read_sensors(&payload);

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx  = s_net_idx,
        .app_idx  = s_app_idx,
        .addr     = GROUP_SENSOR,  /* 0xC000 — bridge subscribed to this */
        .send_ttl = 7,             /* high TTL to reach bridge via relays */
    };

    esp_err_t err = esp_ble_mesh_server_model_send_msg(
        &s_vnd_models[0], &ctx,
        OP_SENSOR_DATA,
        sizeof(payload), (uint8_t *)&payload);

    ESP_LOGI(TAG, "Sent T=%.1f smoke=%.1f batt=%u%% → bridge: %s",
             payload.temperature, payload.smoke,
             payload.battery_pct, esp_err_to_name(err));
}

/* ═══════════════════════════════════════════════════════════
   VENDOR MODEL CALLBACK
   ═══════════════════════════════════════════════════════════ */
static void model_cb(esp_ble_mesh_model_cb_event_t event,
                     esp_ble_mesh_model_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT) return;

    uint32_t opcode = param->model_operation.opcode;
    uint8_t *msg    = param->model_operation.msg;
    uint16_t len    = param->model_operation.length;

    switch (opcode) {

    case OP_SENSOR_ACK:
        if (len > 0)
            ESP_LOGI(TAG, "Bridge ACK: %s",
                     msg[0] == 0x01 ? "✅ forwarded to WiFi" : "⚠️ bridge has no WiFi");
        break;

    case OP_BRIDGE_ADVERT:
        ESP_LOGI(TAG,"ADVERT receives");
        if (len >= sizeof(ble_bridge_advert_t)) {

            const ble_bridge_advert_t *a = (const ble_bridge_advert_t *)msg;
            ESP_LOGI(TAG, "Bridge " MACSTR " wifi=%d layer=%d rssi=%d",
                     MAC2STR(a->bridge_mac),
                     a->wifi_connected, a->wifi_layer, a->rssi_to_root);
        }
        break;

    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════
   CONFIG SERVER CALLBACK
   Bridge sends AppKey Add + Model Bind after provisioning.
   We capture app_idx here so we can publish.
   ═══════════════════════════════════════════════════════════ */
static void config_srv_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                           esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) return;
    ESP_LOGI(TAG, "Provisioner addr: 0x%04X", param->ctx.addr);
    switch (param->ctx.recv_op) {
    case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
        s_app_idx = param->value.state_change.appkey_add.app_idx;
        ESP_LOGI(TAG, "AppKey added: 0x%04X", s_app_idx);
        break;
    case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
        s_app_idx = param->value.state_change.mod_app_bind.app_idx;
        s_config_complete = true;  
        ESP_LOGI(TAG, "Model bound: app_idx=0x%04X — ready to publish!", s_app_idx);
        break;
    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════
   PROV CALLBACK
   ═══════════════════════════════════════════════════════════ */
static void prov_cb(esp_ble_mesh_prov_cb_event_t event,
                    esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {

    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "Prov register err=%d",
                 param->prov_register_comp.err_code);
        break;

    case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "Beacon started — waiting for bridge to provision...");
        break;

    case ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "Bridge is provisioning us...");
        break;

    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        s_provisioned = true;
        s_net_idx     = param->node_prov_complete.net_idx;
        s_my_addr     = param->node_prov_complete.addr;
        ESP_LOGI(TAG, "✅ Provisioned! addr=0x%04X net_idx=0x%04X",
                 s_my_addr, s_net_idx);
        /* app_idx arrives next via config_srv_cb (AppKey Add + Model Bind) */
          /* Subscribe to bridge advert now that we have a valid element address */
    esp_ble_mesh_model_subscribe_group_addr(
        s_my_addr, CID_ESP, VENDOR_MODEL_ID, GROUP_BRIDGE_ADVERT);
        break;

    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        s_provisioned = false;
        s_app_idx     = 0xFFFF;
        ESP_LOGW(TAG, "Reset — restarting beacon");
        esp_ble_mesh_node_prov_enable(
            (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV |
                                         ESP_BLE_MESH_PROV_GATT));
        break;

    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════
   SENSOR TASK
   ═══════════════════════════════════════════════════════════ */
static void sensor_task(void *arg)
{
    /* Random stagger — prevents all nodes publishing simultaneously */
    vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 5000)));
    while (1) {
        publish_sensor();
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PUBLISH_MS));
    }
}

/* ═══════════════════════════════════════════════════════════
   INIT
   ═══════════════════════════════════════════════════════════ */


/* Init BLE stack + mesh models, but do NOT start beacon */
esp_err_t node_init_silent(void)
{
    esp_err_t err;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) return err;

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) return err;

    err = esp_bluedroid_init();
    if (err != ESP_OK) return err;

    err = esp_bluedroid_enable();
    if (err != ESP_OK) return err;

    esp_read_mac(s_my_mac, ESP_MAC_BT);
    s_dev_uuid[0] = 0xEE; s_dev_uuid[1] = 0xEE;
    s_dev_uuid[2] = s_my_mac[0]; s_dev_uuid[3] = s_my_mac[1];
    s_dev_uuid[4] = s_my_mac[2]; s_dev_uuid[5] = s_my_mac[3];
    s_dev_uuid[6] = s_my_mac[4]; s_dev_uuid[7] = s_my_mac[5];

    esp_ble_mesh_register_prov_callback(prov_cb);
    esp_ble_mesh_register_config_server_callback(config_srv_cb);
    esp_ble_mesh_register_custom_model_callback(model_cb);

    err = esp_ble_mesh_init(&s_prov, &s_comp);
    if (err != ESP_OK) return err;

    esp_ble_mesh_set_unprovisioned_device_name("M5-SENSOR");

    if (esp_ble_mesh_node_is_provisioned()) {
        s_provisioned = true;
        s_my_addr     = esp_ble_mesh_get_primary_element_address();
        s_app_idx     = PROV_APP_IDX;
        s_net_idx     = PROV_NET_IDX;
        ESP_LOGI(TAG, "Already provisioned (NVS) addr=0x%04X", s_my_addr);
    }
    /* ← beacon NOT started here */

    xTaskCreatePinnedToCore(sensor_task, "sensor",
        SENSOR_TASK_STACK, NULL, tskIDLE_PRIORITY + 2, &s_sensor_handle, 1);

    ESP_LOGI(TAG, "BLE stack ready (beacon not yet started)");
    return ESP_OK;
}

/* Original node_init = silent init + immediate beacon */
esp_err_t node_init(void)
{
    esp_err_t err = node_init_silent();
    if (err != ESP_OK) return err;

    if (!esp_ble_mesh_node_is_provisioned()) {
        err = esp_ble_mesh_node_prov_enable( // beacon
            ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "node_prov_enable: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}


// esp_err_t node_init(void)
// {
//     esp_err_t err;

//     ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

//     esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
//     err = esp_bt_controller_init(&bt_cfg);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "bt_controller_init: %s", esp_err_to_name(err));
//         return err;
//     }

//     err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "bt_controller_enable: %s", esp_err_to_name(err));
//         return err;
//     }

//     err = esp_bluedroid_init();
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "bluedroid_init: %s", esp_err_to_name(err));
//         return err;
//     }

//     err = esp_bluedroid_enable();
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "bluedroid_enable: %s", esp_err_to_name(err));
//         return err;
//     }

//     /* UUID from BT MAC — unique per device */
//     esp_read_mac(s_my_mac, ESP_MAC_BT);
//     s_dev_uuid[0] = 0xEE; s_dev_uuid[1] = 0xEE; /* 0xEE = sensor node */
//     s_dev_uuid[2] = s_my_mac[0]; s_dev_uuid[3] = s_my_mac[1];
//     s_dev_uuid[4] = s_my_mac[2]; s_dev_uuid[5] = s_my_mac[3];
//     s_dev_uuid[6] = s_my_mac[4]; s_dev_uuid[7] = s_my_mac[5];

//     ESP_LOGI(TAG, "BT MAC: " MACSTR, MAC2STR(s_my_mac));

//     esp_ble_mesh_register_prov_callback(prov_cb);
//     esp_ble_mesh_register_config_server_callback(config_srv_cb);
//     esp_ble_mesh_register_custom_model_callback(model_cb);

//     err = esp_ble_mesh_init(&s_prov, &s_comp);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "esp_ble_mesh_init: %s", esp_err_to_name(err));
//         return err;
//     }

//     esp_ble_mesh_set_unprovisioned_device_name("M5-SENSOR");

//     if (esp_ble_mesh_node_is_provisioned()) {
//         /* Already provisioned from NVS — skip beacon, just start publishing */
//         s_provisioned = true;
//         s_my_addr     = esp_ble_mesh_get_primary_element_address();
//         ESP_LOGI(TAG, "Already provisioned (NVS) addr=0x%04X", s_my_addr);
//         /* app_idx will be restored from NVS automatically by the stack */
//         s_app_idx = PROV_APP_IDX;
//         s_net_idx = PROV_NET_IDX;
//     } else {
//         /* Not provisioned — broadcast beacon, wait for bridge */
//         err = esp_ble_mesh_node_prov_enable(
//             (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV |
//                                          ESP_BLE_MESH_PROV_GATT));
//         if (err != ESP_OK) {
//             ESP_LOGE(TAG, "node_prov_enable: %s", esp_err_to_name(err));
//             return err;
//         }
//     }

//     xTaskCreatePinnedToCore(sensor_task, "sensor",
//         SENSOR_TASK_STACK, NULL, tskIDLE_PRIORITY + 2, &s_sensor_handle, 1);

//     ESP_LOGI(TAG, "M5StickC node ready");
//     return ESP_OK;
// }


/* ═══════════════════════════════════════════════════════════
   node_deinit
   Tears down the BLE node stack in the correct order.
   Call from task context only — never from a BLE callback.
   ═══════════════════════════════════════════════════════════ */
esp_err_t node_deinit(void)
{
    esp_err_t err;

     if (s_sensor_handle) {
        vTaskDelete(s_sensor_handle);
        s_sensor_handle = NULL;
    }
 
    /* 1. Stop advertising / scanning so no new callbacks fire */
    err = esp_ble_mesh_node_prov_disable(
              ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "node_prov_disable: %s", esp_err_to_name(err));
 
    /* Small gap — let any in-flight callbacks drain */
    vTaskDelay(pdMS_TO_TICKS(200));
 
    /* 2. Tear down the BLE Mesh stack */
    esp_ble_mesh_deinit_param_t param = {
    .erase_flash = true  // true if you want to wipe provisioning data from flash
    };
    esp_ble_mesh_deinit(&param);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "ble_mesh_deinit: %s", esp_err_to_name(err));
 
    vTaskDelay(pdMS_TO_TICKS(100));
 
    /* 3. Tear down Bluedroid */
    err = esp_bluedroid_disable();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "bluedroid_disable: %s", esp_err_to_name(err));
 
    err = esp_bluedroid_deinit();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "bluedroid_deinit: %s", esp_err_to_name(err));
 
    /* 4. Tear down BT controller */
    err = esp_bt_controller_disable();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "bt_controller_disable: %s", esp_err_to_name(err));
 
    err = esp_bt_controller_deinit();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "bt_controller_deinit: %s", esp_err_to_name(err));
 
    ESP_LOGI(TAG, "BLE node stack torn down");
    return ESP_OK;
}
 
 