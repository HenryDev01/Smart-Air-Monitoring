#include "ble_bridge.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh.h"
#include "esp_coexist.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../configuration/air_mesh.h"
#include <string.h>

static const char *TAG = "BLE_BRIDGE";

/* ── Identity ─────────────────────────────────────────────── */
// WiFi STA MAC of the node that acts as bridge/scanner
// Change this to match whichever M5Stick is your "root" side
static const uint8_t BRIDGE_MAC[6] = {0xe8, 0x9f, 0x6d, 0x0a, 0x45, 0x9c};

#define OUR_COMPANY_ID  0x02E5
#define ADV_TYPE_SENSOR 0xAB
#define ADV_MAGIC_0  0xA1
#define ADV_MAGIC_1  0xB2

typedef struct __attribute__((packed)) {
    uint16_t company_id;
    uint8_t  pkt_type;
    uint8_t  mac[6];
    uint8_t  magic_0;    
    uint8_t  magic_1;   
    float    temp;
    float    hum;
    float    smoke;
} ble_sensor_adv_t;

/* ── State ────────────────────────────────────────────────── */
static bool     s_bt_ready       = false;
static bool     s_is_bridge      = false;
static bool     s_is_advertising = false;
static bool     s_is_scanning    = false;
static uint8_t  s_my_mac[6]      = {0};

/* ── GAP callback ─────────────────────────────────────────── */
static void gap_cb(esp_gap_ble_cb_event_t event,
                   esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT: {
        // Data is set, now start advertising
        esp_ble_adv_params_t adv_params = {
            .adv_int_min       = 0x20,
            .adv_int_max       = 0x40,
            .adv_type          = ADV_TYPE_NONCONN_IND,
            .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
            .channel_map       = ADV_CHNL_ALL,
            .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        };
        esp_ble_gap_start_advertising(&adv_params);
        break;
    }

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_is_advertising = true;
            ESP_LOGI(TAG, "BLE advertising started");
        } else {
            ESP_LOGE(TAG, "BLE adv start failed: %d",
                     param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_is_advertising = false;
        ESP_LOGI(TAG, "BLE advertising stopped");
        break;

    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(0); // 0 = scan forever
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_is_scanning = true;
            ESP_LOGI(TAG, "BLE scanning started — watching for sensor nodes");
        }
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_is_scanning = false;
        ESP_LOGI(TAG, "BLE scanning stopped");
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;

        uint8_t *adv     = param->scan_rst.ble_adv;
        uint8_t  adv_len = param->scan_rst.adv_data_len;
        uint8_t i = 0;
        
        while (i + 1 < adv_len) {
            uint8_t len  = adv[i];
            if (len == 0 || i + len >= adv_len) break;
            uint8_t type = adv[i + 1];

            if (type == 0xFF && len >= sizeof(ble_sensor_adv_t)) {
                ble_sensor_adv_t *pkt = (ble_sensor_adv_t *)&adv[i + 2];

                if (pkt->company_id != OUR_COMPANY_ID ||
                    pkt->pkt_type   != ADV_TYPE_SENSOR ||
                    pkt->magic_0    != ADV_MAGIC_0     ||
                    pkt->magic_1    != ADV_MAGIC_1) {
                    i += len + 1;
                    continue;
                }
                ESP_LOGI(TAG, "BLE sensor from " MACSTR
                    " | T:%.1f H:%.1f S:%.1f",
                    MAC2STR(pkt->mac),
                    pkt->temp, pkt->hum, pkt->smoke);

                mesh_addr_t parent;
                bool wifi_up = (esp_mesh_get_parent_bssid(&parent) == ESP_OK
                               && parent.addr[0] != 0);
                if (wifi_up) {
                    pkt_sensor_t mesh_pkt = {
                        .hdr         = { .type = PKT_SENSOR_DATA },
                        .temperature = pkt->temp,
                        .smoke       = pkt->smoke,
                        .hop_count   = 1,
                        .etx_to_root = 1.0f,
                    };
                    memcpy(mesh_pkt.hdr.src_id, pkt->mac, 6);


                mesh_data_t tx = {
                        .data  = (uint8_t *)&mesh_pkt,
                        .size  = sizeof(mesh_pkt),
                        .proto = MESH_PROTO_BIN,
                        .tos   = MESH_TOS_P2P,
                    };
                    esp_err_t err = esp_mesh_send(NULL, &tx,
                                                  MESH_DATA_TODS, NULL, 0);
                    ESP_LOGI(TAG, "Forwarded to WiFi mesh: %s",
                             esp_err_to_name(err));
                }
            }
            i += len + 1;
        }
        break;
    }
    default:
        break;
    }
}

/* ── BT stack init ────────────────────────────────────────── */
static esp_err_t bt_init(void)
{
    esp_err_t ret;

    // Free classic BT memory since we only use BLE
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mem_release: %s", esp_err_to_name(ret));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) { ESP_LOGE(TAG, "bt_controller_init: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) { ESP_LOGE(TAG, "bt_controller_enable: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_bluedroid_init();
    if (ret) { ESP_LOGE(TAG, "bluedroid_init: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_bluedroid_enable();
    if (ret) { ESP_LOGE(TAG, "bluedroid_enable: %s", esp_err_to_name(ret)); return ret; }

    ESP_LOGI(TAG, "BT stack ready");
    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────────── */
esp_err_t ble_bridge_init(void)
{
    esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);
    s_is_bridge = (memcmp(s_my_mac, BRIDGE_MAC, 6) == 0);

    ESP_LOGI(TAG, "MAC: " MACSTR " → BLE role: %s",
             MAC2STR(s_my_mac),
             s_is_bridge ? "SCANNER/BRIDGE" : "ADVERTISER/NODE");

    // Coexistence
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);

    esp_err_t ret = bt_init();
    if (ret != ESP_OK) return ret;

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));

    s_bt_ready = true;

    // Start immediately — don't wait for WiFi mesh events
    // Bridge node scans, sensor nodes advertise
    if (s_is_bridge) {
        ble_bridge_start_scanning();
    } else {
        // Start advertising with dummy values — sensor task will update
        ble_advertise_sensor(0.0f, 0.0f, 0.0f);
    }

    return ESP_OK;
}

esp_err_t ble_advertise_sensor(float temp, float hum, float smoke)
{
    if (!s_bt_ready) return ESP_ERR_INVALID_STATE;

    ble_sensor_adv_t payload = {
        .company_id = OUR_COMPANY_ID,
        .pkt_type   = ADV_TYPE_SENSOR,
        .magic_0    = ADV_MAGIC_0,    
        .magic_1    = ADV_MAGIC_1,    
        .temp       = temp,
        .hum        = hum,
        .smoke      = smoke,
    };
    memcpy(payload.mac, s_my_mac, 6);

    uint8_t adv_data[31] = {0};
    uint8_t payload_len  = sizeof(ble_sensor_adv_t);
    adv_data[0] = payload_len + 1;
    adv_data[1] = 0xFF;
    memcpy(&adv_data[2], &payload, payload_len);

    // Stop first if already advertising, then update data
    if (s_is_advertising) {
        esp_ble_gap_stop_advertising();
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    esp_ble_gap_config_adv_data_raw(adv_data, payload_len + 2);
    // Advertising starts automatically in GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT

    ESP_LOGI(TAG, "BLE adv update T:%.1f H:%.1f S:%.1f", temp, hum, smoke);
    return ESP_OK;
}

void ble_node_start_advertising(void)
{
    if (!s_bt_ready || s_is_advertising) return;
    ble_advertise_sensor(0.0f, 0.0f, 0.0f); // sensor task will update soon
}

void ble_node_stop_advertising(void)
{
    if (!s_bt_ready || !s_is_advertising) return;
    esp_ble_gap_stop_advertising();
}

void ble_bridge_start_scanning(void)
{
    if (!s_bt_ready || s_is_scanning) return;
    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    esp_ble_gap_set_scan_params(&scan_params);
    // Scanning starts in GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT
}

void ble_bridge_stop_scanning(void)
{
    if (!s_bt_ready || !s_is_scanning) return;
    esp_ble_gap_stop_scanning();
}

bool ble_bridge_is_scanning(void) { return s_is_scanning; }