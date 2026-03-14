#include "esp_wifi.h"
#include "esp_mesh.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "esp_coexist.h"
#include "../ble_mesh/ble_bridge.h"
#include "../../configuration/air_mesh.h"
#include "../routing/mesh_routing.h"
#include "../flooding/mesh_gossip.h"
#include "../auth/mesh_auth.h"
#include "../../air_mqtt/air_mqtt.h"

static const char *TAG = "MESH_INIT";

static esp_netif_t *s_netif_sta  = NULL;
static esp_netif_t *s_netif_ap   = NULL;
static TaskHandle_t s_recv_task_handle = NULL;
static int s_kick_count = 0;
static bool s_mqtt_initialized = false;

#define MAX_KICK_COUNT  3

// helper to publish status of a connected child
static void publish_child_status(const uint8_t *mac, const char *status)
{
    if (!esp_mesh_is_root()) return;

    // get parent of that node — for direct children, parent is root itself
    uint8_t self_mac[6];
    esp_read_mac(self_mac, ESP_MAC_WIFI_STA);

    // get RSSI of child
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
        .layer         = esp_mesh_get_layer() + 1, // child is one layer below root
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
    vTaskDelay(pdMS_TO_TICKS(3000));  // wait 3s for DNS to be ready
      esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_netif_sta, &ip_info);
    ESP_LOGI("MQTT", "IP: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI("MQTT", "GW: " IPSTR, IP2STR(&ip_info.gw));
    mqtt_init();
    vTaskDelete(NULL);  // delete self after done
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
        break;

    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *cc = event_data;
        ESP_LOGI(TAG, "Child connected: " MACSTR " (aid=%d)",
                 MAC2STR(cc->mac), cc->aid);
        

        // /* Root issues an auth challenge to newly connected child */
        // if (esp_mesh_is_root()) {
        //     ESP_LOGI(TAG, "Sending auth challenge to " MACSTR " (aid=%d)", MAC2STR(cc->mac), cc->aid);
        //     auth_send_challenge(cc->mac);
        // }

        break;
    }

    case MESH_EVENT_CHILD_DISCONNECTED:
        mesh_event_child_disconnected_t *dc = event_data;
        ESP_LOGW(TAG, "Child disconnected");
        publish_child_status(dc->mac, "offline");  // ← publish offline status
        break;

    case MESH_EVENT_PARENT_CONNECTED: {
        layer = esp_mesh_get_layer();
        ESP_LOGI(TAG, "Connected to parent. Layer: %d", layer);
          /* Send join request to authenticate with root */
        auth_send_join_request();
        routing_reset_etx();

            // root connected to router — start DHCP
        if (esp_mesh_is_root()) {
            ESP_LOGI(TAG, "Root — starting DHCP");
            esp_netif_dhcpc_stop(s_netif_sta);
            esp_netif_dhcpc_start(s_netif_sta);  // ← triggers IP_EVENT_STA_GOT_IP
        }

   
        // if(!esp_mesh_is_root())
        // {
        //       mesh_data_t data;
        //     char msg[] = "HELLO_MESH FROM CHILD";
        //     data.data = (uint8_t *)msg;
        //     data.size = strlen(msg) + 1;

        //     // NULL = send to root
        //     esp_err_t err = esp_mesh_send(NULL, &data, 0, NULL, 0);
        //     ESP_LOGI(TAG, "Send to root result: %s", esp_err_to_name(err));
        // }
        break;
    }

    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disc = (mesh_event_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Parent disconnected, reason: %d", disc->reason);
        routing_invalidate_parent();
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
        // FIX: compare the value inside the struct, not the pointer
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

    case MESH_EVENT_ROOT_ADDRESS: {
        break;
    }

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
        
        if(esp_mesh_is_root() && !s_mqtt_initialized)
        {
            s_mqtt_initialized = true;
            mqtt_init();
        }
   
    }
}



static void mesh_recv_task(void *arg)
{
    mesh_addr_t from,to;
    mesh_data_t data;
    uint8_t self_mac[6] = {0};
    uint8_t     buf[sizeof(pkt_gossip_t) + 32];
    int         flag = 0;

    esp_read_mac(self_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "Receive task started");
    while (1) {
        data.data = buf;
        data.size = sizeof(buf);

        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        // esp_err_t err = esp_mesh_recv_toDS(&from, &to,&data, portMAX_DELAY, &flag, NULL, 0);

      
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_mesh_recv error: %s", esp_err_to_name(err));
            continue;
        }

     
        // ESP_LOGI(TAG, "Received %d bytes from " MACSTR, data.size, MAC2STR(from.addr));
        // ESP_LOGI(TAG, "ROOT Received: %s", (char *)data.data);
        
       
        if (data.size < sizeof(pkt_hdr_t)) continue;
       
        pkt_hdr_t *hdr = (pkt_hdr_t *)buf;

        switch (hdr->type) {
            case PKT_HELLO:

                if (memcmp(from.addr, self_mac, 6) == 0) {
                    ESP_LOGW("HELLO", "Ignoring self-sent packet");
                    break;
                }
                ESP_LOGI(TAG, "HELLO received from " MACSTR,
                MAC2STR(from.addr));

                // since pkt hello struct is pkt header it can convert to pkt hello
                //[ pkt_hdr_t ][ hello_payload ]
                
                routing_handle_hello((pkt_hello_t *)hdr, &from);
                break;
            case PKT_SENSOR_DATA: // here send to mqtt for root.

                // only publish if node is authenticated
                if (!auth_is_node_authenticated(from.addr)) {
                    ESP_LOGW(TAG, "Dropping unauthenticated sensor data from " MACSTR,
                            MAC2STR(from.addr));
                    break;
                }
                pkt_sensor_t *pkt = (pkt_sensor_t *)hdr;
                ESP_LOGI(TAG, "Sensor data from " MACSTR, MAC2STR(from.addr));
                ESP_LOGI(TAG, "  Temperature: %.1f°C", pkt->temperature);
                ESP_LOGI(TAG, "  Smoke: %.1f%%", pkt->smoke);
                ESP_LOGI(TAG, "  ETX to root: %.2f", pkt->etx_to_root);
                ESP_LOGI(TAG, "  Hop count: %d", pkt->hop_count);

                if (esp_mesh_is_root()) {
                    mqtt_publish_sensor(from.addr, pkt->temperature, pkt->smoke,
                                        pkt->etx_to_root, pkt->hop_count);
                }
        
                break;
            case PKT_GOSSIP:
                // ESP_LOGI("GOSSIP", "FROM  : " MACSTR, MAC2STR(from.addr));
                // ESP_LOGI("GOSSIP", "SELF  : " MACSTR, MAC2STR(self_mac));
                // if (memcmp(from.addr, self_mac, 6) == 0) {
                //     ESP_LOGW("GOSSIP", "Ignoring self-sent packet");
                //     break;
                // }
                //ESP_LOGI("GOSSIP", "Gossip received from " MACSTR, MAC2STR(from.addr));
                gossip_handle_packet((pkt_gossip_t *)hdr);
                break;
            case PKT_JOIN_REQUEST:
              if (esp_mesh_is_root())
                auth_handle_join_request((pkt_join_req_t *)buf, &from);
              break;
            case PKT_JOIN_RESPONSE:
                /* Non-root receives this — auth module processes it */
                auth_handle_join_response((pkt_join_resp_t *)buf);
            break;
             case PKT_CHALLENGE_RESP:
                ESP_LOGI(TAG, "Challenge response received from " MACSTR,
                         MAC2STR(from.addr));
                if (esp_mesh_is_root())
                    auth_handle_challenge_response((pkt_challenge_resp_t *)buf,
                                                &from);
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
                esp_mesh_disconnect(); // leave the tree
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

    /* 0. WiFi/BLE coexistence — must be before WiFi and BT init */
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    ESP_LOGI(TAG, "Coexistence: BALANCE");

    /* 1. NVS */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. TCP/IP stack + event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(
        esp_netif_create_default_wifi_mesh_netifs(&s_netif_sta, &s_netif_ap));

    /* 3. WiFi driver */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));// can cause packet not received. will need to test later when implementing power saving


    /* 4. Mesh init FIRST, then register handlers */  
    ESP_ERROR_CHECK(esp_mesh_init());

    ESP_ERROR_CHECK(esp_event_handler_register(
        MESH_EVENT, ESP_EVENT_ANY_ID, mesh_event_handler, NULL));  // only once
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL));
    //auth_set_authenticated_cb(on_node_authenticated);  


    /* 5. Mesh config */
    mesh_cfg_t mcfg = MESH_INIT_CONFIG_DEFAULT();

    uint8_t mesh_id[6] = MESH_ID;
    memcpy(mcfg.mesh_id.addr, mesh_id, 6);

    mcfg.channel = 0;  // FIX: 0 = auto-detect router channel

    mcfg.mesh_ap.max_connection = MESH_AP_MAX_CONN;
    strlcpy((char *)mcfg.mesh_ap.password, MESH_PASSWORD,
            sizeof(mcfg.mesh_ap.password));

    // Only the device that becomes ROOT will successfully use it.
    mcfg.router.ssid_len = strlen(MESH_ROUTER_SSID);
    memcpy(mcfg.router.ssid, MESH_ROUTER_SSID, mcfg.router.ssid_len);
    memcpy(mcfg.router.password, MESH_ROUTER_PASSWORD, strlen(MESH_ROUTER_PASSWORD));

    ESP_LOGI(TAG, "Mesh ID: " MACSTR, MAC2STR(mcfg.mesh_id.addr));
    ESP_LOGI(TAG, "Router SSID: %s", mcfg.router.ssid);

    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYERS));
    ESP_ERROR_CHECK(esp_mesh_set_config(&mcfg));

    /* 6. Self-organized BEFORE start */  // FIX: moved here from event handler
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));

    /* 7. Power save */
    //ESP_ERROR_CHECK(esp_mesh_enable_ps());  // can cause packet not received. will need to test later when implementing power saving

    /* 8. Start mesh */
    ESP_ERROR_CHECK(esp_mesh_start());

    mesh_addr_t group_id = {{0x01,0x00,0x00,0x00,0x00,0x01}};
    esp_mesh_set_group_id(&group_id, 1);
    ESP_LOGI(TAG, "Joined mesh group");

    ESP_LOGI(TAG, "Mesh started — IDF %s | channel=auto maxLayer=%d",
             esp_get_idf_version(), MESH_MAX_LAYERS);

    /* 9. Receive task */
    xTaskCreatePinnedToCore(mesh_recv_task, "mesh_recv", 4096, NULL,
                            configMAX_PRIORITIES - 1, &s_recv_task_handle, 0);
    /* 10. BLE mesh — after WiFi mesh is stable */
    esp_err_t ble_ret = ble_bridge_init();
    
    if (ble_ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE mesh init failed: %s (non-fatal)",
                 esp_err_to_name(ble_ret));
    } else {
        ESP_LOGI(TAG, "BLE mesh init OK");
    }

    return ESP_OK;
}