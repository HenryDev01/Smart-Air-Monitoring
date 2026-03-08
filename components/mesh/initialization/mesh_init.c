#include "esp_wifi.h"
#include "esp_mesh.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "../../configuration/air_mesh.h"
#include "../routing/mesh_routing.h"
#include "esp_coexist.h"
#include "ble_mesh_init.h"
#include "../display/display.h"

static const char *TAG = "MESH_INIT";

static esp_netif_t *s_netif_sta  = NULL;
static esp_netif_t *s_netif_ap   = NULL;
static TaskHandle_t s_recv_task_handle = NULL;

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

        break;
    }

    case MESH_EVENT_CHILD_DISCONNECTED:
        ESP_LOGW(TAG, "Child disconnected");
        break;

    case MESH_EVENT_PARENT_CONNECTED: {
        layer = esp_mesh_get_layer();
        ESP_LOGI(TAG, "Connected to parent. Layer: %d", layer);


        mesh_addr_t group_id = {{0x01,0x00,0x00,0x00,0x00,0x01}};
        esp_mesh_set_group_id(&group_id, 1);
        ESP_LOGI(TAG, "Joined mesh group");

        if(!esp_mesh_is_root())
        {
              mesh_data_t data;
            char msg[] = "HELLO_MESH FROM CHILD";
            data.data = (uint8_t *)msg;
            data.size = strlen(msg) + 1;

            // NULL = send to root
            esp_err_t err = esp_mesh_send(NULL, &data, 0, NULL, 0);
            ESP_LOGI(TAG, "Send to root result: %s", esp_err_to_name(err));
        }

        /* ── BLE mesh init once WiFi mesh is connected ── */  
        esp_err_t ble_ret = ble_mesh_init();
        if (ble_ret != ESP_OK) {
            ESP_LOGE(TAG, "BLE mesh init failed: %s",
                    esp_err_to_name(ble_ret));
        } else {
            ESP_LOGI(TAG, "BLE mesh init OK");
            }
        }                                                      
        break;
    
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
                 toDS == MESH_TODS_REACHABLE ? "UP" : "DOWN");
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
        display_print(10, 85, "WiFi OK!", COLOR_GREEN, COLOR_BLACK);
    }
}

static void mesh_recv_task(void *arg)
{
    mesh_addr_t from,to;
    mesh_data_t data;
    uint8_t     buf[sizeof(pkt_gossip_t) + 32];
    int         flag = 0;

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
               ESP_LOGI(TAG, "HELLO received from " MACSTR,
                 MAC2STR(from.addr));

                // since pkt hello struct is pkt header it can convert to pkt hello
                //[ pkt_hdr_t ][ hello_payload ]
                
                routing_handle_hello((pkt_hello_t *)hdr, &from);
                break;
            case PKT_SENSOR_DATA:
                pkt_sensor_t *pkt = (pkt_sensor_t *)hdr;
                ESP_LOGI(TAG, "Sensor data from " MACSTR, MAC2STR(from.addr));
                ESP_LOGI(TAG, "  Temperature: %.1f°C", pkt->temperature);
                ESP_LOGI(TAG, "  Humidity: %.1f%%", pkt->humidity);
                ESP_LOGI(TAG, "  Smoke: %.1f%%", pkt->smoke);
                ESP_LOGI(TAG, "  ETX to root: %.2f", pkt->etx_to_root);
                ESP_LOGI(TAG, "  Hop count: %d", pkt->hop_count);
        
                break;
            case PKT_GOSSIP:
                break;
            case PKT_JOIN_REQUEST:
                break;
            case PKT_CHALLENGE_RESP:
                break;
            case PKT_CHALLENGE:
                break;
            case PKT_SESSION_REVOKE:
                ESP_LOGW(TAG, "Session revoked by root — re-authenticating");
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

    /* 0. WiFi + BLE coexistence */
    ret = esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Coex preference set failed: %s", esp_err_to_name(ret));
        /* non-fatal — continue */
    }
    ESP_LOGI(TAG, "WiFi/BLE coexistence enabled (balanced)");

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

    /* 4. Mesh init FIRST, then register handlers */  // FIX: order matters
    ESP_ERROR_CHECK(esp_mesh_init());

    ESP_ERROR_CHECK(esp_event_handler_register(
        MESH_EVENT, ESP_EVENT_ANY_ID, mesh_event_handler, NULL));  // only once
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL));

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
    //ESP_ERROR_CHECK(esp_mesh_enable_ps());

    /* 8. Start mesh */
    ESP_ERROR_CHECK(esp_mesh_start());

    ESP_LOGI(TAG, "Mesh started — IDF %s | channel=auto maxLayer=%d",
             esp_get_idf_version(), MESH_MAX_LAYERS);

    /* 9. Receive task */
    xTaskCreatePinnedToCore(mesh_recv_task, "mesh_recv", 4096, NULL,
                            configMAX_PRIORITIES - 1, &s_recv_task_handle, 0);

    return ESP_OK;
}