/*
 * ble_bridge.c
 * ─────────────
 * ESP32 bridge = BLE Mesh PROVISIONER + WiFi Mesh NODE
 *
 * - Auto-provisions any M5StickC that advertises an unprovisioned beacon
 * - Receives OP_SENSOR_DATA from provisioned M5StickC nodes via BLE mesh
 * - Forwards received data upstream through WiFi mesh toward root
 * - Periodically sends OP_BRIDGE_ADVERT so M5StickC nodes know bridge is alive
 */

#include "ble_bridge.h"

#include "esp_bt_main.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_coexist.h"
#include "esp_bt.h"
#include "esp_mac.h"
#include "esp_mesh.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_log.h"

#include "../../configuration/air_ble_mesh.h"
#include "../../configuration/air_mesh.h"

static const char *TAG = "BLE_BRIDGE";

#define BRIDGE_ADVERT_MS  5000
#define ADVERT_TASK_STACK 2048
#define MAX_RETRIES       5
#define RETRY_DELAY_MS    2000

/* ── State ───────────────────────────────────────────────── */
static uint8_t  s_dev_uuid[16] = {0};
static uint8_t  s_my_mac[6]    = {0};
static uint8_t  s_bridge_load  = 0;
static uint32_t s_fwd_seq      = 0;

static inline bool wifi_mesh_up(void)
{
    return is_mesh_connected || esp_mesh_is_root();
}

/* ── Config state machine ────────────────────────────────── */
typedef enum {
    CFG_STATE_APPKEY,
    CFG_STATE_MODELBIND,
    CFG_STATE_DONE,
} cfg_state_t;

typedef struct {
    uint16_t    addr;
    cfg_state_t state;
    uint8_t     retries;
} cfg_job_t;

typedef struct {
    uint16_t addr;
    uint32_t opcode;
    bool     success;
} cfg_result_t;

//task related
static TaskHandle_t s_vendor_msg_task_handle  = NULL;
static TaskHandle_t s_config_node_task_handle = NULL;
static TaskHandle_t s_advert_task_handle      = NULL;

// this is to send let the ble data to seperate task with seperated larger memory (stack) to process because BLE mesh has small stack.
static QueueHandle_t s_cfg_queue  = NULL;
static QueueHandle_t s_cfg_result = NULL;

/* ── Vendor message queue ────────────────────────────────── */
/* One flat struct covers every outbound vendor message.
   data[] holds the raw payload; len says how many bytes.
   Largest payload is ble_bridge_advert_t — that sets the buffer size. */
#define VMSG_DATA_MAX  sizeof(ble_bridge_advert_t)

typedef struct {
    uint32_t               opcode;
    esp_ble_mesh_msg_ctx_t ctx;
    uint16_t               len;
    uint8_t                data[VMSG_DATA_MAX];
} vmsg_t;

static QueueHandle_t s_msg_queue = NULL;

/* ── Prov struct — provisioner, fixed addr ───────────────── */
static esp_ble_mesh_prov_t s_prov = {
    .uuid               = s_dev_uuid,
    .output_size        = 0,
    .output_actions     = 0,
    .input_size         = 0,
    .prov_unicast_addr  = 0x0001,   /* bridge's own BLE mesh address */
    .prov_start_address = 0x0005,   /* first address assigned to M5StickC nodes */
};

/* ── Composition ─────────────────────────────────────────── */
static esp_ble_mesh_cfg_srv_t s_cfg_srv = {
    .relay            = ESP_BLE_MESH_RELAY_ENABLED,
    .beacon           = ESP_BLE_MESH_BEACON_ENABLED,
    .friend_state     = ESP_BLE_MESH_FRIEND_ENABLED,
    .gatt_proxy       = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .default_ttl      = 7,
    .net_transmit     = ESP_BLE_MESH_TRANSMIT(4, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(4, 20),
};

static esp_ble_mesh_client_t s_cfg_client;

static esp_ble_mesh_model_op_t s_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(OP_SENSOR_DATA,   sizeof(ble_sensor_payload_t)),
    ESP_BLE_MESH_MODEL_OP(OP_BRIDGE_SELECT, sizeof(ble_bridge_select_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(s_vnd_pub, 2 + sizeof(ble_bridge_advert_t), ROLE_PROVISIONER);

static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_cfg_srv),
    ESP_BLE_MESH_MODEL_CFG_CLI(&s_cfg_client),
};
static esp_ble_mesh_model_t s_vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, VENDOR_MODEL_ID,
                              s_vnd_ops, &s_vnd_pub, NULL),
};
static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, s_vnd_models),
};
static esp_ble_mesh_comp_t s_comp = {
    .cid = CID_ESP, .pid = 0x0003, .vid = 0x0001,
    .element_count = ARRAY_SIZE(s_elements),
    .elements      = s_elements,
};

/* ═══════════════════════════════════════════════════════════
   FORWARD BLE SENSOR DATA → WIFI MESH → ROOT
   ═══════════════════════════════════════════════════════════ */
static void forward_to_wifi_mesh(const ble_sensor_payload_t *ble)
{
    if (!wifi_mesh_up()) {
        ESP_LOGW(TAG, "WiFi mesh not up — cannot forward, dropping packet");
        return;
    }

    pkt_sensor_t pkt = {
        .hdr         = { .type = PKT_SENSOR_DATA, .seq = s_fwd_seq++, .origin = SRC_BLE },
        .temperature = ble->temperature,
        .smoke       = ble->smoke,
        .hop_count   = (uint8_t)(esp_mesh_get_layer() + 1),
        .etx_to_root = 1.0f,
    };
    memcpy(pkt.hdr.src_id, ble->src_mac, 6);

    mesh_data_t tx = {
        .data  = (uint8_t *)&pkt,
        .size  = sizeof(pkt),
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };

    esp_err_t err = esp_mesh_send(NULL, &tx, MESH_DATA_TODS, NULL, 0);
    /* No float formatting inside BLE callback — dtoa can assert on small stack */
    int t_int  = (int)ble->temperature;
    int t_frac = (int)((ble->temperature - t_int) * 10);
    int s_int  = (int)ble->smoke;
    int s_frac = (int)((ble->smoke - s_int) * 10);
    ESP_LOGI(TAG, "BLE→WiFi " MACSTR " T=%d.%d smoke=%d.%d batt=%u%%: %s",
             MAC2STR(ble->src_mac),
             t_int, t_frac, s_int, s_frac, ble->battery_pct,
             esp_err_to_name(err));
}

/* ═══════════════════════════════════════════════════════════
   SEND APPKEY TO NEWLY PROVISIONED NODE
   Never call from a BLE stack callback.
   ═══════════════════════════════════════════════════════════ */
static void send_appkey_to_node(uint16_t addr)
{
    uint8_t app_key[16] = PROV_APP_KEY;

    esp_ble_mesh_cfg_client_set_state_t set = {0};
    set.app_key_add.net_idx = PROV_NET_IDX;
    set.app_key_add.app_idx = PROV_APP_IDX;
    memcpy(set.app_key_add.app_key, app_key, 16);

    esp_ble_mesh_client_common_param_t common = {
        .opcode       = ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD,
        .model        = &s_root_models[1],
        .ctx.net_idx  = PROV_NET_IDX,
        .ctx.app_idx  = 0,
        .ctx.addr     = addr,
        .ctx.send_ttl = 3,
        .msg_timeout  = 20000,
    };
    esp_err_t e = esp_ble_mesh_config_client_set_state(&common, &set);
    ESP_LOGI(TAG, "AppKey → 0x%04x: %s", addr, esp_err_to_name(e));
}

/* ═══════════════════════════════════════════════════════════
   BIND VENDOR MODEL APPKEY ON NODE
   Never call from a BLE stack callback.
   ═══════════════════════════════════════════════════════════ */
static void send_model_bind_to_node(uint16_t addr)
{
    esp_ble_mesh_cfg_client_set_state_t set = {0};
    set.model_app_bind.element_addr  = addr;
    set.model_app_bind.model_app_idx = PROV_APP_IDX;
    set.model_app_bind.model_id      = VENDOR_MODEL_ID;
    set.model_app_bind.company_id    = CID_ESP;

    esp_ble_mesh_client_common_param_t common = {
        .opcode       = ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND,
        .model        = &s_root_models[1],
        .ctx.net_idx  = PROV_NET_IDX,
        .ctx.app_idx  = 0,
        .ctx.addr     = addr,
        .ctx.send_ttl = 3,
        .msg_timeout  = 20000,
    };
    esp_err_t e = esp_ble_mesh_config_client_set_state(&common, &set);
    ESP_LOGI(TAG, "Model bind → 0x%04x: %s", addr, esp_err_to_name(e));
}

/* ═══════════════════════════════════════════════════════════
   VENDOR MESSAGE TASK
   Dequeues vendor messages one by one and calls
   esp_ble_mesh_server_model_send_msg from task context —
   never from inside a BLE stack callback.
   ═══════════════════════════════════════════════════════════ */
static void vendor_msg_task(void *arg)
{
    vmsg_t msg;
    while (1) {
        if (xQueueReceive(s_msg_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        esp_err_t e = esp_ble_mesh_server_model_send_msg(
            &s_vnd_models[0], &msg.ctx,
            msg.opcode, msg.len, msg.data);

        if (e != ESP_OK)
            ESP_LOGW(TAG, "vendor send op=0x%06"PRIx32" err=%s",
                     msg.opcode, esp_err_to_name(e));
    }
}

/* ═══════════════════════════════════════════════════════════
   CONFIG NODE TASK — state machine with retry
   Single persistent task. Serialises all post-prov config work
   so we never send segmented messages from inside a callback.
   ═══════════════════════════════════════════════════════════ */
static void config_node_task(void *arg)
{
    cfg_job_t job;

    while (1) {
        if (xQueueReceive(s_cfg_queue, &job, portMAX_DELAY) != pdTRUE)
            continue;

        ESP_LOGI(TAG, "Config job: addr=0x%04x state=%d", job.addr, job.state);

        while (job.state != CFG_STATE_DONE && job.retries < MAX_RETRIES) {

            if (job.state == CFG_STATE_APPKEY)
                send_appkey_to_node(job.addr);
            else if (job.state == CFG_STATE_MODELBIND)
                send_model_bind_to_node(job.addr);

            /* Wait for config_client_cb to post the result.
               Timeout is slightly longer than msg_timeout (10 s). */
            cfg_result_t result;
            if (xQueueReceive(s_cfg_result, &result, pdMS_TO_TICKS(22000)) != pdTRUE) {
                job.retries++;
                ESP_LOGW(TAG, "No result for 0x%04x state=%d retry=%d/%d",
                         job.addr, job.state, job.retries, MAX_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
                continue;
            }

            /* Result may be for a different addr if two nodes provision
               simultaneously — put it back and wait again. */
            if (result.addr != job.addr) {
                xQueueSend(s_cfg_result, &result, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            if (!result.success) {
                job.retries++;
                ESP_LOGW(TAG, "Config failed 0x%04x state=%d retry=%d/%d",
                         job.addr, job.state, job.retries, MAX_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
                continue;
            }

            /* Advance state on success */
            if (job.state == CFG_STATE_APPKEY) {
                ESP_LOGI(TAG, "AppKey OK 0x%04x -> binding model", job.addr);
                job.state   = CFG_STATE_MODELBIND;
                job.retries = 0;
            } else if (job.state == CFG_STATE_MODELBIND) {
                ESP_LOGI(TAG, "Node 0x%04x fully configured", job.addr);
                job.state = CFG_STATE_DONE;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (job.retries >= MAX_RETRIES)
            ESP_LOGE(TAG, "Giving up on 0x%04x after %d retries",
                     job.addr, MAX_RETRIES);
    }
}

/* ═══════════════════════════════════════════════════════════
   CONFIG CLIENT CALLBACK
   Only posts results to the queue — never sends from here.
   ═══════════════════════════════════════════════════════════ */
static void config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                              esp_ble_mesh_cfg_client_cb_param_t *param)
{
    if (!s_cfg_result) return;

    uint16_t addr   = param->params->ctx.addr;
    uint32_t opcode = param->params->opcode;

    cfg_result_t result = {
        .addr    = addr,
        .opcode  = opcode,
        .success = false,
    };

    switch (event) {
    case ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT:
        result.success = (param->error_code == 0);
        ESP_LOGI(TAG, "Config result addr=0x%04x op=0x%04"PRIx32" ok=%d",
                 addr, opcode, result.success);
        xQueueSend(s_cfg_result, &result, 0);
        break;

    case ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT:
        result.success = false;
        ESP_LOGW(TAG, "Config timeout addr=0x%04x op=0x%04"PRIx32, addr, opcode);
        xQueueSend(s_cfg_result, &result, 0);
        break;

    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════
   VENDOR MODEL CALLBACK
   Only enqueues work — never sends directly.
   ═══════════════════════════════════════════════════════════ */
static void model_cb(esp_ble_mesh_model_cb_event_t event,
                     esp_ble_mesh_model_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT) return;

    uint32_t opcode = param->model_operation.opcode;
    uint16_t len    = param->model_operation.length;
    uint8_t *msg    = param->model_operation.msg;

    /* Deep-copy ctx — pointer is only valid during this callback */
    esp_ble_mesh_msg_ctx_t ctx;
    memcpy(&ctx, param->model_operation.ctx, sizeof(ctx));

    switch (opcode) {

    case OP_SENSOR_DATA: {
        if (len < sizeof(ble_sensor_payload_t)) {
            ESP_LOGW(TAG, "Short payload (%u B)", len); break;
        }
        const ble_sensor_payload_t *p = (const ble_sensor_payload_t *)msg;

        forward_to_wifi_mesh(p);

        /* Enqueue ACK — never send from inside a BLE stack callback */
        uint8_t ack = wifi_mesh_up() ? 0x01 : 0x00;
        vmsg_t vmsg = {
            .opcode = OP_SENSOR_ACK,
            .ctx    = ctx,
            .len    = sizeof(ack),
        };
        vmsg.data[0] = ack;
        if (xQueueSend(s_msg_queue, &vmsg, 0) != pdTRUE)
            ESP_LOGW(TAG, "msg_queue full — ACK dropped");
        break;
    }

    case OP_BRIDGE_SELECT: {
        if (len < sizeof(ble_bridge_select_t)) break;
        const ble_bridge_select_t *sel = (const ble_bridge_select_t *)msg;
        if (sel->selected) s_bridge_load++;
        else if (s_bridge_load > 0) s_bridge_load--;
        ESP_LOGI(TAG, "%s by " MACSTR " (load=%u)",
                 sel->selected ? "Selected" : "Deselected",
                 MAC2STR(sel->sensor_mac), s_bridge_load);
        break;
    }

    default:
        ESP_LOGD(TAG, "Unhandled opcode 0x%06"PRIx32, opcode);
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
        ESP_LOGI(TAG, "Prov register err=%d", param->prov_register_comp.err_code);
        break;

    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "Provisioner scanning for M5StickC nodes...");
        break;

    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        esp_ble_mesh_unprov_dev_add_t add = {0};
        memcpy(add.addr, param->provisioner_recv_unprov_adv_pkt.addr, 6);
        add.addr_type = param->provisioner_recv_unprov_adv_pkt.addr_type;
        memcpy(add.uuid, param->provisioner_recv_unprov_adv_pkt.dev_uuid, 16);
        add.bearer   = ESP_BLE_MESH_PROV_ADV;
        add.oob_info = 0;
        esp_err_t e = esp_ble_mesh_provisioner_add_unprov_dev(&add,
            ADD_DEV_RM_AFTER_PROV_FLAG |
            ADD_DEV_START_PROV_NOW_FLAG |
            ADD_DEV_FLUSHABLE_DEV_FLAG);
        ESP_LOGI(TAG, "Provisioning " MACSTR ": %s",
                 MAC2STR(add.addr), esp_err_to_name(e));
        break;
    }

    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: {
        uint16_t addr = param->provisioner_prov_complete.unicast_addr;
        ESP_LOGI(TAG, "Provisioned 0x%04x — queuing config job", addr);

        /* Small delay so node can store provisioning keys before we
           start sending config messages */
        vTaskDelay(pdMS_TO_TICKS(2000));

        cfg_job_t job = {
            .addr    = addr,
            .state   = CFG_STATE_APPKEY,
            .retries = 0,
        };
        xQueueSend(s_cfg_queue, &job, 0);
        break;
    }

    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════
   BRIDGE ADVERTISEMENT TASK
   ═══════════════════════════════════════════════════════════ */
static void send_bridge_advert(void)
{
    int8_t rssi = -127;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    ble_bridge_advert_t advert = {
        .wifi_connected = wifi_mesh_up() ? 1 : 0,
        .wifi_layer     = (uint8_t)esp_mesh_get_layer(),
        .load           = s_bridge_load,
        .rssi_to_root   = rssi,
    };
    memcpy(advert.bridge_mac, s_my_mac, 6);

    vmsg_t vmsg = {
        .opcode = OP_BRIDGE_ADVERT,
        .ctx    = {
            .net_idx  = PROV_NET_IDX,
            .app_idx  = PROV_APP_IDX,
            .addr     = GROUP_BRIDGE_ADVERT,
            .send_ttl = 3,
        },
        .len = sizeof(advert),
    };
    memcpy(vmsg.data, &advert, sizeof(advert));

    if (xQueueSend(s_msg_queue, &vmsg, 0) != pdTRUE)
        ESP_LOGW(TAG, "msg_queue full — advert dropped");
    else
        ESP_LOGD(TAG, "Advert queued: wifi=%d layer=%d load=%d rssi=%d",
                 advert.wifi_connected, advert.wifi_layer,
                 advert.load, rssi);
}

static void bridge_advert_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500 + (esp_random() % 3000)));
    while (1) {
        send_bridge_advert();
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_ADVERT_MS));
    }
}

/* ═══════════════════════════════════════════════════════════
   PUBLIC QUERIES
   ═══════════════════════════════════════════════════════════ */
bool    ble_bridge_is_provisioned(void) { return true; }
uint8_t ble_bridge_get_load(void)       { return s_bridge_load; }

/* ═══════════════════════════════════════════════════════════
   PUBLIC: INIT
   ═══════════════════════════════════════════════════════════ */
esp_err_t ble_bridge_init(void)
{
    esp_err_t err;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_init: %s", esp_err_to_name(err)); return err;
    }

    ESP_ERROR_CHECK(esp_coex_preference_set(ESP_COEX_PREFER_BALANCE));

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_enable: %s", esp_err_to_name(err)); return err;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_init: %s", esp_err_to_name(err)); return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_enable: %s", esp_err_to_name(err)); return err;
    }

    /* UUID from BT MAC */
    esp_read_mac(s_my_mac, ESP_MAC_BT);
    s_dev_uuid[0] = 0xDD; s_dev_uuid[1] = 0xDD;
    s_dev_uuid[2] = s_my_mac[0]; s_dev_uuid[3] = s_my_mac[1];
    s_dev_uuid[4] = s_my_mac[2]; s_dev_uuid[5] = s_my_mac[3];
    s_dev_uuid[6] = s_my_mac[4]; s_dev_uuid[7] = s_my_mac[5];

    ESP_LOGI(TAG, "BT MAC: " MACSTR, MAC2STR(s_my_mac));

    /* Create all queues and tasks before registering callbacks.
       Keep depths small — vmsg_t is large and each slot costs heap. */
    s_cfg_queue  = xQueueCreate(4, sizeof(cfg_job_t));
    s_cfg_result = xQueueCreate(2, sizeof(cfg_result_t));
    s_msg_queue  = xQueueCreate(4, sizeof(vmsg_t));
    if (!s_cfg_queue || !s_cfg_result || !s_msg_queue) {
        ESP_LOGE(TAG, "Failed to create queues");
        return ESP_ERR_NO_MEM;
    }

    xTaskCreatePinnedToCore(vendor_msg_task, "vnd_msg",
        2048, NULL, tskIDLE_PRIORITY + 2, &s_vendor_msg_task_handle, 1);

    xTaskCreatePinnedToCore(config_node_task, "cfg_node",
        3072, NULL, tskIDLE_PRIORITY + 3, &s_config_node_task_handle, 1);

    esp_ble_mesh_register_prov_callback(prov_cb);
    esp_ble_mesh_register_custom_model_callback(model_cb);
    esp_ble_mesh_register_config_client_callback(config_client_cb);

    err = esp_ble_mesh_init(&s_prov, &s_comp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_mesh_init: %s", esp_err_to_name(err)); return err;
    }

    err = esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioner_prov_enable: %s", esp_err_to_name(err)); return err;
    }

    uint8_t net_key[16] = PROV_NET_KEY;
    err = esp_ble_mesh_provisioner_add_local_net_key(net_key, PROV_NET_IDX);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "add_net_key: %s", esp_err_to_name(err)); return err;
    }

    uint8_t app_key[16] = PROV_APP_KEY;
    err = esp_ble_mesh_provisioner_add_local_app_key(app_key, PROV_NET_IDX, PROV_APP_IDX);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "add_app_key: %s", esp_err_to_name(err)); return err;
    }

    err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
        s_prov.prov_unicast_addr, PROV_APP_IDX, VENDOR_MODEL_ID, CID_ESP);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bind local model: %s", esp_err_to_name(err));
    }

    esp_ble_mesh_model_subscribe_group_addr(
        s_prov.prov_unicast_addr, CID_ESP, VENDOR_MODEL_ID, GROUP_SENSOR);

    xTaskCreatePinnedToCore(bridge_advert_task, "ble_advert",
        ADVERT_TASK_STACK, NULL, tskIDLE_PRIORITY + 1, &s_advert_task_handle, 1);

    ESP_LOGI(TAG, "BLE bridge READY — provisioner active, forwarding BLE->WiFi");
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════
   ble_bridge_deinit
   Tears down the BLE bridge (provisioner) stack.
   Also deletes the FreeRTOS queues created in ble_bridge_init.
   Call from task context only — never from a BLE callback.
   ═══════════════════════════════════════════════════════════ */
esp_err_t ble_bridge_deinit(void)
{
    esp_err_t err;
 
    /* 1. Stop provisioner scanning so no new prov callbacks fire */
    err = esp_ble_mesh_provisioner_prov_disable(ESP_BLE_MESH_PROV_ADV);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "provisioner_prov_disable: %s", esp_err_to_name(err));
 
    vTaskDelay(pdMS_TO_TICKS(200));
 
    /* 2. Tear down BLE Mesh stack */
    esp_ble_mesh_deinit_param_t param = {
    .erase_flash = true  // true if you want to wipe provisioning data from flash
    };
    err = esp_ble_mesh_deinit(&param);
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
    
    //tear down task
    if (s_advert_task_handle)      { vTaskDelete(s_advert_task_handle);      s_advert_task_handle      = NULL; }
    if (s_vendor_msg_task_handle)  { vTaskDelete(s_vendor_msg_task_handle);  s_vendor_msg_task_handle  = NULL; }
    if (s_config_node_task_handle) { vTaskDelete(s_config_node_task_handle); s_config_node_task_handle = NULL; }

    /* 5. Delete queues — they will be recreated on next ble_bridge_init */
    if (s_cfg_queue)  { vQueueDelete(s_cfg_queue);  s_cfg_queue  = NULL; }
    if (s_cfg_result) { vQueueDelete(s_cfg_result); s_cfg_result = NULL; }
    if (s_msg_queue)  { vQueueDelete(s_msg_queue);  s_msg_queue  = NULL; }
 
    /* 6. Reset runtime state */
    s_bridge_load = 0;
    s_fwd_seq     = 0;
 
    ESP_LOGI(TAG, "BLE bridge stack torn down");
    return ESP_OK;
}