#include "esp_mesh.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"


#include "../configuration/air_mesh.h"
#include "../mesh/routing/mesh_routing.h"
#include "../utils/utils.h"
#include "../mesh/auth/mesh_auth.h"
#include "../mesh/flooding/mesh_gossip.h"

static uint8_t s_temp_max  = 35;
static uint8_t s_smoke_max = 40;

static uint32_t s_sensor_seq = 0;
static uint8_t  s_my_mac[6]  = {0};
static const char *TAG = "SENSOR";


static float read_temperature(void)
{
    // TODO: Replace with SHT31/DHT22 I2C driver
    return 28.0f + (esp_random() % 50) * 0.1f; // stub
}
static float read_mq2_smoke(void)
{
    // mock
    return 18.0f + (esp_random() % 100) * 0.1f; // stub
}

static void send_sensor_reading(void *arg)
{
    (void)arg; /* unused parameter */

    // need to check whcih node has sensor

    while (1) {

        // Wait until mesh is up and we have a parent
        if (!esp_mesh_is_device_active()) {
            ESP_LOGW(TAG, "Device is not active, waiting...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "Sensor task started");
        float temp  = read_temperature();
        float smoke = read_mq2_smoke();

        //alert
          uint8_t alert_flags = 0;
        if ((uint8_t)temp  > s_temp_max)  alert_flags |= 0x01;
        if ((uint8_t)smoke > s_smoke_max) alert_flags |= 0x02;

        if (alert_flags) {
            ESP_LOGW(TAG, "⚠️ ALERT — temp=%.1f (max=%d) smoke=%.1f (max=%d)",
                     temp, s_temp_max, smoke, s_smoke_max);

            sensor_cfg_t alert = {
                .type        = CFG_TYPE_ALERT,
                .alert_flags = alert_flags,
                .temp_val    = temp,
                .smoke_val   = smoke,
            };
            memcpy(alert.src_mac, s_my_mac, 6);
            gossip_send((uint8_t *)&alert, sizeof(alert), GOSSIP_TTL_DEFAULT);
        }


        // sensor packet
        pkt_sensor_t pkt = {
            .hdr = {
                .type = PKT_SENSOR_DATA,
                .seq  = s_sensor_seq++,
            },
            .temperature = temp,
            .smoke    = smoke,
            .hop_count   = (uint8_t)esp_mesh_get_layer(),
            .etx_to_root = routing_get_etx_to_root(),
        };
        memcpy(pkt.hdr.src_id, s_my_mac, 6);

        // esp-mesh struct
        mesh_data_t data = {
            .data  = (uint8_t *)&pkt,
            .size  = sizeof(pkt),
            .proto = MESH_PROTO_BIN,
            .tos   = MESH_TOS_P2P,
        };
        
        //send to root
        esp_err_t err = esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);

        ESP_LOGI(TAG, "I am " MACSTR ", layer %d",
         MAC2STR(s_my_mac),
         esp_mesh_get_layer());

        if (!esp_mesh_is_root()) {
            mesh_addr_t parent;
            uint8_t parent_sta_mac[6];

            esp_err_t parent_err = esp_mesh_get_parent_bssid(&parent);
            
            ESP_LOGI(TAG, "My parent is " MACSTR "",
                MAC2STR(parent.addr));
            if (parent_err == ESP_OK) {

                ap_to_sta_mac(parent.addr, parent_sta_mac);

                ESP_LOGI(TAG, "Parent AP: " MACSTR, MAC2STR(parent.addr));
                ESP_LOGI(TAG, "Parent STA: " MACSTR, MAC2STR(parent_sta_mac));
                routing_record_tx(parent_sta_mac, err == ESP_OK); // true=success, false=fail
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Send failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS)); 
    }

}
void sensor_set_thresholds(uint8_t temp_max, uint8_t smoke_max)
{
    s_temp_max  = temp_max;
    s_smoke_max = smoke_max;
    ESP_LOGI(TAG, "Thresholds updated — temp=%d smoke=%d", temp_max, smoke_max);
}

void sensor_init(void)
{
    esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "Sensor init — MAC " MACSTR, MAC2STR(s_my_mac));

    xTaskCreatePinnedToCore(send_sensor_reading,
                            "sensor_task",
                            4096,
                            NULL,
                            tskIDLE_PRIORITY + 2,
                            NULL,
                            1);
}