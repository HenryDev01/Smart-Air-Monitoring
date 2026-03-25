#include "esp_wifi.h"
#include "esp_mesh.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/event_groups.h"

#include "../../configuration/air_mesh.h"
#include "../routing/mesh_routing.h"
#include "../flooding/mesh_gossip.h"
#include "../auth/mesh_auth.h"
#include "../../air_mqtt/air_mqtt.h"
#include "../../ble_mesh/bridge/ble_bridge.h"

static const char *TAG = "MESH_INIT";

static esp_netif_t *s_netif_sta  = NULL;
static esp_netif_t *s_netif_ap   = NULL;

static TaskHandle_t s_recv_task_handle = NULL;

static int s_kick_count = 0;
static bool s_mqtt_initialized = false;
bool is_mesh_connected = false;
static bool s_root_has_router = false;  // ← track router connectivity for root
static bool s_netif_created = false;

static EventGroupHandle_t s_mesh_event_group = NULL;

#define MAX_KICK_COUNT   3
#define MESH_RUNNING_BIT BIT0

/* ═══════════════════════════════════════════════════════════
   PUBLIC: mesh_is_healthy
   Returns true if this node has a working uplink.
   - Root   → must have router connection
   - Others → must be connected to parent
   ═══════════════════════════════════════════════════════════ */
bool mesh_is_healthy(void)
{
    bool is_root = esp_mesh_is_root();

    ESP_LOGI("HEALTH", "is_root=%d s_root_has_router=%d is_mesh_connected=%d",
             is_root, s_root_has_router, is_mesh_connected);

    if (esp_mesh_is_root()) {
        return s_root_has_router;  // ← root uses router flag
    }
    return is_mesh_connected;      // ← non-root uses parent flag
}

// helper to publish status of a connected child
static void publish_child_status(const uint8_t *mac, const char *status)
{
    if (!esp_mesh_is_root()) return;

    uint8_t self_mac[6];
    esp_read_mac(self_mac, ESP_MAC_WIFI_STA);

    wifi_sta_list_t sta_list;
    esp_wifi_ap_get_sta_list(&sta_list);
    int rssi = 0;
    for (int i = 0; i < sta_list.num; i++) {
        if (memcmp(sta_list.sta[i].mac, mac, 6) == 0) {
            rssi = sta_list.sta[i].rssi;
            break;
        }
    }

    node_status_t info = {
        .mac           = mac,
        .status        = status,
        .layer         = esp_mesh_get_layer() + 1,
        .parent_mac    = self_mac,
        .rssi          = rssi,
        .authenticated = auth_is_node_authenticated(mac),
    };

    mqtt_publish_node_status(&info);
}

static void on_node_authenticated(const uint8_t *mac)
{
    publish_child_status(mac, "online");
}

static void mqtt_start_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_netif_sta, &ip_info);
    ESP_LOGI("MQTT", "IP: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI("MQTT", "GW: " IPSTR, IP2STR(&ip_info.gw));
    mqtt_init();
    vTaskDelete(NULL);
}

static void mesh_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    mesh_addr_t id = {0};
    static int layer = 0;

    switch (event_id) {

    case MESH_EVENT_STARTED:
        esp_mesh_get_id(&id);
        ESP_LOGI(TAG, "Mesh started. ID: " MACSTR, MAC2STR(id.addr));
        break;

    case MESH_EVENT_STOPPED:
        ESP_LOGW(TAG, "Mesh stopped");
        is_mesh_connected  = false;
        s_root_has_router  = false;  // ← reset on stop
        break;

    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *cc = event_data;
        ESP_LOGI(TAG, "Child connected: " MACSTR " (aid=%d)",
                 MAC2STR(cc->mac), cc->aid);
        break;
    }

    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *dc = event_data;
        ESP_LOGW(TAG, "Child disconnected");
        publish_child_status(dc->mac, "offline");
        break;
    }

    case MESH_EVENT_PARENT_CONNECTED: {
        is_mesh_connected = true;
        layer = esp_mesh_get_layer();
        ESP_LOGI(TAG, "Connected to parent. Layer: %d", layer);
        auth_send_join_request();
        routing_reset_etx();

        if (esp_mesh_is_root()) {
            ESP_LOGI(TAG, "Root — starting DHCP");
            esp_netif_dhcpc_stop(s_netif_sta);
            esp_netif_dhcpc_start(s_netif_sta);
            // s_root_has_router set true in IP_EVENT_STA_GOT_IP
        }
        break;
    }

    case MESH_EVENT_PARENT_DISCONNECTED: {
        is_mesh_connected = false;
        mesh_event_disconnected_t *disc = (mesh_event_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Parent disconnected, reason: %d", disc->reason);
        routing_invalidate_parent();

        if (esp_mesh_is_root()) {
            s_root_has_router = false;  // ← router gone
            ESP_LOGW(TAG, "Root lost router connection");
        }
        break;
    }

    case MESH_EVENT_ROUTING_TABLE_ADD: {
        mesh_event_routing_table_change_t *rt = event_data;
        ESP_LOGI(TAG, "Routing table +%d nodes (total=%d)",
                 rt->rt_size_change, rt->rt_size_new);
        break;
    }

    case MESH_EVENT_ROUTING_TABLE_REMOVE:
        ESP_LOGW(TAG, "Routing table updated (-nodes)");
        break;

    case MESH_EVENT_TODS_STATE: {
        mesh_event_toDS_state_t *toDS = event_data;
        ESP_LOGI(TAG, "ToDS: %s",
                 *toDS == MESH_TODS_REACHABLE ? "UP" : "DOWN");
        break;
    }

    case MESH_EVENT_VOTE_STARTED:
        ESP_LOGI(TAG, "Root election started");
        break;

    case MESH_EVENT_VOTE_STOPPED:
        ESP_LOGI(TAG, "Root election finished");
        break;

    case MESH_EVENT_ROOT_SWITCH_REQ:
        ESP_LOGI(TAG, "Root switch requested");
        break;

    case MESH_EVENT_ROOT_ADDRESS:
        break;

    default:
        ESP_LOGD(TAG, "Unhandled mesh event: %ld", event_id);
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = data;
        ESP_LOGI(TAG, "Root got IP: " IPSTR, IP2STR(&evt->ip_info.ip));

        if (esp_mesh_is_root()) {
            s_root_has_router = true;  // ← router confirmed up
            ESP_LOGI(TAG, "Root has router connection");

            if (!s_mqtt_initialized) {

                s_mqtt_initialized = true;
                esp_err_t mqtt_err =  mqtt_init();
                if(mqtt_err != ESP_OK)
                    ESP_LOGW(TAG, "MQTT error: %s", esp_err_to_name(mqtt_err));

            }
        }
    }

    if (id == IP_EVENT_STA_LOST_IP) {
        if (esp_mesh_is_root()) {
            s_root_has_router = false;  // ← router lost
            ESP_LOGW(TAG, "Root lost IP — router gone");
        }
        if(s_mqtt_initialized)
        {   
            ESP_LOGI(TAG,"MQTT DEINIT CALLED");
            s_mqtt_initialized = false;
            mqtt_deinit();
        }
    }
}

static void mesh_recv_task(void *arg)
{
    mesh_addr_t from;
    mesh_data_t data;
    uint8_t self_mac[6] = {0};
    uint8_t buf[sizeof(pkt_gossip_t) + 32];
    int flag = 0;

    esp_read_mac(self_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "Receive task started");

    while (1) {
        xEventGroupWaitBits(s_mesh_event_group, MESH_RUNNING_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        data.data = buf;
        data.size = sizeof(buf);

        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_mesh_recv error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (data.size < sizeof(pkt_hdr_t)) continue;

        pkt_hdr_t *hdr = (pkt_hdr_t *)buf;

        switch (hdr->type) {
            case PKT_HELLO:
                if (memcmp(from.addr, self_mac, 6) == 0) {
                    //ESP_LOGW("HELLO", "Ignoring self-sent packet");
                    break;
                }
                ESP_LOGI(TAG, "HELLO received from " MACSTR, MAC2STR(from.addr));
                routing_handle_hello((pkt_hello_t *)hdr, &from);
                break;

            case PKT_SENSOR_DATA:
                if (!auth_is_node_authenticated(from.addr)) {
                    ESP_LOGW(TAG, "Dropping unauthenticated sensor data from " MACSTR,
                             MAC2STR(from.addr));
                    break;
                }
                pkt_sensor_t *pkt = (pkt_sensor_t *)hdr;
                const char *origin = (pkt->hdr.origin == SRC_BLE) ? "BLE" :
                                     (pkt->hdr.origin == SRC_WIFI) ? "WIFI" : "UNKNOWN";
                ESP_LOGI(TAG, "Sensor data from " MACSTR " via %s",
                         MAC2STR(pkt->hdr.src_id), origin);
                ESP_LOGI(TAG, "  Temperature: %.1f°C", pkt->temperature);
                ESP_LOGI(TAG, "  Smoke: %.1f%%", pkt->smoke);
                ESP_LOGI(TAG, "  ETX to root: %.2f", pkt->etx_to_root);
                ESP_LOGI(TAG, "  Hop count: %d", pkt->hop_count);
                if (esp_mesh_is_root()) {
                    mqtt_publish_sensor(pkt->hdr.src_id, pkt->temperature,
                                        pkt->smoke, pkt->etx_to_root, pkt->hop_count);
                }
                break;

            case PKT_GOSSIP:
                gossip_handle_packet((pkt_gossip_t *)hdr);
                break;

            case PKT_JOIN_REQUEST:
                if (esp_mesh_is_root())
                    auth_handle_join_request((pkt_join_req_t *)buf, &from);
                break;

            case PKT_JOIN_RESPONSE:
                auth_handle_join_response((pkt_join_resp_t *)buf);
                break;

            case PKT_CHALLENGE_RESP:
                ESP_LOGI(TAG, "Challenge response received from " MACSTR,
                         MAC2STR(from.addr));
                if (esp_mesh_is_root())
                    auth_handle_challenge_response((pkt_challenge_resp_t *)buf, &from);
                break;

            case PKT_CHALLENGE:
                auth_handle_challenge((pkt_challenge_t *)buf);
                break;

            case PKT_SESSION_REVOKE:
                ESP_LOGW(TAG, "Session revoked by root — re-authenticating");
                auth_send_join_request();
                break;

            case PKT_KICK:
                ESP_LOGW(TAG, "Kicked by root — disconnecting from mesh");
                esp_mesh_disconnect();
                break;

            default:
                ESP_LOGD(TAG, "Unknown packet type: 0x%02X", hdr->type);
                break;
        }
    }
}

esp_err_t mesh_init(void)
{
    esp_err_t ret;

    /* 1. NVS */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. TCP/IP stack + event loop — one time only */
    if (!s_netif_created) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        ESP_ERROR_CHECK(
            esp_netif_create_default_wifi_mesh_netifs(&s_netif_sta, &s_netif_ap));
        s_netif_created = true;
    }

    /* 3. WiFi driver */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* 4. Mesh init then register handlers */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(
        MESH_EVENT, ESP_EVENT_ANY_ID, mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(        // ← register lost IP
        IP_EVENT, IP_EVENT_STA_LOST_IP, ip_event_handler, NULL));

    /* 5. Mesh config */
    mesh_cfg_t mcfg = MESH_INIT_CONFIG_DEFAULT();
    uint8_t mesh_id[6] = MESH_ID;
    memcpy(mcfg.mesh_id.addr, mesh_id, 6);
    mcfg.channel = 0;
    mcfg.mesh_ap.max_connection = MESH_AP_MAX_CONN;
    strlcpy((char *)mcfg.mesh_ap.password, MESH_PASSWORD,
            sizeof(mcfg.mesh_ap.password));
    mcfg.router.ssid_len = strlen(MESH_ROUTER_SSID);
    memcpy(mcfg.router.ssid, MESH_ROUTER_SSID, mcfg.router.ssid_len);
    memcpy(mcfg.router.password, MESH_ROUTER_PASSWORD,
           strlen(MESH_ROUTER_PASSWORD));

    ESP_LOGI(TAG, "Mesh ID: " MACSTR, MAC2STR(mcfg.mesh_id.addr));
    ESP_LOGI(TAG, "Router SSID: %s", mcfg.router.ssid);

    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYERS));
    ESP_ERROR_CHECK(esp_mesh_set_config(&mcfg));
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));

    /* 6. Start mesh */
    ESP_ERROR_CHECK(esp_mesh_start());

    mesh_addr_t group_id = {{0x01, 0x00, 0x00, 0x00, 0x00, 0x01}};
    esp_mesh_set_group_id(&group_id, 1);
    ESP_LOGI(TAG, "Joined mesh group");
    ESP_LOGI(TAG, "Mesh started — IDF %s | channel=auto maxLayer=%d",
             esp_get_idf_version(), MESH_MAX_LAYERS);

    /* 7. Event group + recv task — created once, reused on re-init */
    if (s_mesh_event_group == NULL) {
        s_mesh_event_group = xEventGroupCreate();
    }
    if (s_recv_task_handle == NULL) {
        xTaskCreatePinnedToCore(mesh_recv_task, "mesh_recv", 4096, NULL,
                                configMAX_PRIORITIES - 1,
                                &s_recv_task_handle, 0);
    }

    /* 8. Signal task that mesh is ready */
    xEventGroupSetBits(s_mesh_event_group, MESH_RUNNING_BIT);

    return ESP_OK;
}

void mesh_deinit(void)
{
    if (s_mesh_event_group != NULL) {
        xEventGroupClearBits(s_mesh_event_group, MESH_RUNNING_BIT);
    }
    is_mesh_connected = false;
    s_root_has_router = false;   // ← reset router flag on deinit
    s_mqtt_initialized = false;  // ← allow MQTT to re-init next time
}