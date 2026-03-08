#include "ble_mesh_init.h"
#include "esp_log.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_mac.h"
#include <string.h>

static const char *TAG = "BLE_MESH";

/* ── state ────────────────────────────────────────────────── */
static ble_mesh_role_t  s_role       = BLE_MESH_ROLE_NODE;
static bool             s_ready      = false;
static uint8_t          s_my_mac[6]  = {0};

/* ── keys (shared across all nodes, same firmware) ───────── */
static uint8_t s_dev_uuid[16] = { 0 };  // filled from MAC at init

static struct esp_ble_mesh_key {
    uint8_t net_key[16];
    uint8_t app_key[16];
} s_keys = {
    .net_key = {
        0xAB, 0x72, 0x4D, 0x6F, 0x6E, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x07, 0x08, 0x09, 0x0A
    },
    .app_key = {
        0x41, 0x69, 0x72, 0x4D, 0x6F, 0x6E,
        0x41, 0x70, 0x70, 0x4B, 0x65, 0x79,
        0x00, 0x01, 0x02, 0x03
    },
};

/* ── vendor model opcodes ─────────────────────────────────── */
static esp_ble_mesh_model_op_t s_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(BLE_MESH_VND_OP_SENSOR,     1),
    ESP_BLE_MESH_MODEL_OP(BLE_MESH_VND_OP_ALERT,      1),
    ESP_BLE_MESH_MODEL_OP(BLE_MESH_VND_OP_HELLO,      1),
    ESP_BLE_MESH_MODEL_OP(BLE_MESH_VND_OP_SENSOR_ACK, 1),
    ESP_BLE_MESH_MODEL_OP_END,
};

/* ── config server (required on every node) ──────────────── */
static esp_ble_mesh_cfg_srv_t s_cfg_srv = {
    .relay            = ESP_BLE_MESH_RELAY_ENABLED,
    .beacon           = ESP_BLE_MESH_BEACON_ENABLED,
    .friend_state     = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .gatt_proxy       = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .default_ttl      = 7,
    .net_transmit     = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
};

/* ── vendor models ───────────────────────────────────────── */
static esp_ble_mesh_model_t s_vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(
        BLE_MESH_COMPANY_ID,
        BLE_MESH_SENSOR_MODEL_ID,
        s_vnd_ops, NULL, NULL),
};

/* ── root models array ───────────────────────────────────── */
static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_cfg_srv),
};

/* ── element ─────────────────────────────────────────────── */
static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, s_vnd_models),
};

/* ── composition data ────────────────────────────────────── */
static esp_ble_mesh_comp_t s_comp = {
    .cid        = BLE_MESH_COMPANY_ID,
    .pid        = BLE_MESH_PRODUCT_ID,
    .vid        = BLE_MESH_VERSION_ID,
    .element_count = ARRAY_SIZE(s_elements),
    .elements      = s_elements,
};

/* ── provisioning ─────────────────────────────────────────── */
static esp_ble_mesh_prov_t s_prov = {
    .uuid          = s_dev_uuid,
    .output_size   = 0,
    .output_actions= 0,
    .input_size    = 0,
    .input_actions = 0,
};

/* ── provisioning callback ───────────────────────────────── */
static void prov_complete_cb(uint16_t net_idx, uint16_t addr,
                              uint8_t flags, uint32_t iv_index)
{
    ESP_LOGI(TAG, "Provisioning complete — addr=0x%04x net_idx=%d",
             addr, net_idx);
    s_ready = true;
}

static void prov_event_handler(esp_ble_mesh_prov_cb_event_t event,
                                esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "Prov register complete, err=%d",
                 param->prov_register_comp.err_code);
        break;

    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        prov_complete_cb(
            param->node_prov_complete.net_idx,
            param->node_prov_complete.addr,
            param->node_prov_complete.flags,
            param->node_prov_complete.iv_index);
        break;

    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        ESP_LOGW(TAG, "Node reset — unprovisioned");
        s_ready = false;
        break;

    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "Provisioner: node provisioned addr=0x%04x",
                 param->provisioner_prov_complete.unicast_addr);
        break;

    case ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT:
        ESP_LOGI(TAG, "Provisioner: unprovisioned dev added err=%d",
                 param->provisioner_add_unprov_dev_comp.err_code);
        break;

    default:
        ESP_LOGD(TAG, "Unhandled prov event: %d", event);
        break;
    }
}

/* ── vendor model receive callback ──────────────────────── */
static void vnd_model_recv_cb(esp_ble_mesh_model_cb_event_t event,
                               esp_ble_mesh_model_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT: {
        uint32_t op  = param->model_operation.opcode;
        uint8_t *buf = param->model_operation.msg;
        uint16_t len = param->model_operation.length;

        if (op == BLE_MESH_VND_OP_SENSOR && len >= sizeof(pkt_sensor_t)) {
            pkt_sensor_t *pkt = (pkt_sensor_t *)buf;
            ESP_LOGI(TAG, "[BLE] Sensor from " MACSTR
                     " Temp=%.1f Hum=%.1f Smoke=%.1f",
                     MAC2STR(pkt->hdr.src_id),
                     pkt->temperature,
                     pkt->humidity,
                     pkt->smoke);

        } else if (op == BLE_MESH_VND_OP_ALERT) {
            ESP_LOGW(TAG, "[BLE] Alert received: %.*s", len, (char *)buf);

        } else if (op == BLE_MESH_VND_OP_HELLO) {
            ESP_LOGI(TAG, "[BLE] Hello received len=%d", len);
        }
        break;
    }
    case ESP_BLE_MESH_MODEL_SEND_COMP_EVT:
        if (param->model_send_comp.err_code) {
            ESP_LOGW(TAG, "Model send failed: %d",
                     param->model_send_comp.err_code);
        }
        break;

    default:
        break;
    }
}

/* ── public: send sensor data over BLE mesh ─────────────── */
esp_err_t ble_mesh_send_sensor(float temperature, float humidity, float smoke)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "BLE mesh not ready, skipping sensor send");
        return ESP_ERR_INVALID_STATE;
    }

    pkt_sensor_t pkt = {
        .hdr = { .type = PKT_SENSOR_DATA },
        .temperature = temperature,
        .humidity    = humidity,
        .smoke       = smoke,
    };
    memcpy(pkt.hdr.src_id, s_my_mac, 6);

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx  = BLE_MESH_NET_KEY_IDX,
        .app_idx  = BLE_MESH_APP_KEY_IDX,
        .addr     = BLE_MESH_GROUP_ADDR,   // multicast to all nodes
        .send_ttl = AIR_MESH_TTL_DEFAULT,
    };

    return esp_ble_mesh_model_publish(
        &s_vnd_models[0],
        BLE_MESH_VND_OP_SENSOR,
        sizeof(pkt),
        (uint8_t *)&pkt,
        ROLE_NODE);
}

/* ── public: send alert over BLE mesh ───────────────────── */
esp_err_t ble_mesh_send_alert(const char *msg, uint8_t len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx  = BLE_MESH_NET_KEY_IDX,
        .app_idx  = BLE_MESH_APP_KEY_IDX,
        .addr     = BLE_MESH_GROUP_ADDR,
        .send_ttl = BLE_MESH_TTL_DEFAULT,
    };

    return esp_ble_mesh_model_publish(
        &s_vnd_models[0],
        BLE_MESH_VND_OP_ALERT,
        len,
        (uint8_t *)msg,
        ROLE_NODE);
}

/* ── public: send hello over BLE mesh ───────────────────── */
esp_err_t ble_mesh_send_hello(float etx_to_root, uint8_t hop_count)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t buf[5];
    memcpy(buf, &etx_to_root, 4);
    buf[4] = hop_count;

    return esp_ble_mesh_model_publish(
        &s_vnd_models[0],
        BLE_MESH_VND_OP_HELLO,
        sizeof(buf),
        buf,
        ROLE_NODE);
}

/* ── public: getters ─────────────────────────────────────── */
ble_mesh_role_t ble_mesh_get_role(void) { return s_role; }
bool ble_mesh_is_ready(void)            { return s_ready; }

/* ── public: init ────────────────────────────────────────── */
esp_err_t ble_mesh_init(void)
{
    esp_err_t ret;

    /* 1. Get MAC for UUID */
    esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);
    memcpy(s_dev_uuid, s_my_mac, 6);   // first 6 bytes = MAC
    s_dev_uuid[6] = 0xAB;              // rest = project identifier
    s_dev_uuid[7] = 0x72;
    s_dev_uuid[8] = 0x4D;

    /* 2. Role: root node becomes provisioner */
    if (esp_mesh_is_root()) {
        s_role = BLE_MESH_ROLE_PROVISIONER;
        ESP_LOGI(TAG, "BLE Mesh role: PROVISIONER");
    } else {
        s_role = BLE_MESH_ROLE_NODE;
        ESP_LOGI(TAG, "BLE Mesh role: NODE");
    }

    /* 3. Register callbacks */
    esp_ble_mesh_register_prov_callback(prov_event_handler);
    esp_ble_mesh_register_custom_model_callback(vnd_model_recv_cb);

    /* 4. Init BLE mesh stack */
    ret = esp_ble_mesh_init(&s_prov, &s_comp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_mesh_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 5. Provisioner: add net + app key */
    if (s_role == BLE_MESH_ROLE_PROVISIONER) {
        ESP_ERROR_CHECK(esp_ble_mesh_provisioner_prov_enable(
            ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
        ESP_ERROR_CHECK(esp_ble_mesh_provisioner_add_local_net_key(
            s_keys.net_key, BLE_MESH_NET_KEY_IDX));
        ESP_ERROR_CHECK(esp_ble_mesh_provisioner_add_local_app_key(
            s_keys.app_key, BLE_MESH_NET_KEY_IDX, BLE_MESH_APP_KEY_IDX));
        s_ready = true;
        ESP_LOGI(TAG, "Provisioner ready");
    } else {
        /* Node: enable unprovisioned advertising */
        ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(
            ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
        ESP_LOGI(TAG, "Node advertising for provisioning...");
    }

    ESP_LOGI(TAG, "BLE Mesh init OK — UUID: " MACSTR,
             MAC2STR(s_dev_uuid));
    return ESP_OK;
}