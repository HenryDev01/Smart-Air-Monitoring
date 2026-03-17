#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "../components/mesh/initialization/mesh_init.h"
#include "../components/mesh/routing/mesh_routing.h"
#include "../components/display/display.h"
#include "../components/sensor/sensor.h"
#include "../components/mesh/flooding/mesh_gossip.h"
#include "../components/mesh/auth/mesh_auth.h"
#include "../components/ble_mesh/bridge/ble_bridge.h"
#include "../components/ble_mesh/node/node.h"

static const char *TAG = "m5stick";

void app_main(void)
{   
    bool is_wifi_Connected = false;
    uint8_t self_mac[6];
    char mac_str[32];
    esp_read_mac(self_mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, sizeof(mac_str), "MAC: " MACSTR, MAC2STR(self_mac));

    // // for testing/filtering
    // if (self_mac[5] == 0x98) {
    //     is_wifi_Connected = true;
    // }

    static const uint8_t BRIDGE_WIFI_MAC[6] = {0xac, 0x0b, 0xfb, 0x6f, 0x9c, 0x04};
    is_wifi_Connected = (memcmp(self_mac, BRIDGE_WIFI_MAC, 6) == 0);


    ESP_LOGI(TAG, "╔══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Air Quality Mesh Monitor        ║");
    ESP_LOGI(TAG, "║  ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "╚══════════════════════════════════╝");

    ESP_LOGI(TAG, "MAC: " MACSTR " → role: %s",
         MAC2STR(self_mac),
         is_wifi_Connected ? "BRIDGE (WiFi+BLE)" : "BLE NODE");

    // display on lcd causing overflow

    // display_init();
    // display_clear(COLOR_BLACK);
    // display_print(10, 20, "AirMonitor",  COLOR_BLUE,  COLOR_GREEN);
    // display_print(10, 25, mac_str, COLOR_CYAN, COLOR_BLACK);    

    if(is_wifi_Connected)
    {

                    /* 1. Mesh + WiFi */
            ESP_ERROR_CHECK(mesh_init());

            // if(esp_mesh_is_root()) {
            //     display_print(10, 30, "Role: ROOT", COLOR_YELLOW, COLOR_BLACK);
            // } else {
            //     display_print(10, 30, "Role: NODE", COLOR_YELLOW, COLOR_BLACK);
            // }
            
            auth_init();
            
            routing_init();

            gossip_init();

            sensor_init();

    }
    
      /* 1. NVS */
          esp_err_t ret;



    vTaskDelay(pdMS_TO_TICKS(3000)); // wait for WiFi to connect (if provisioned)
    if(is_wifi_Connected)
    {
        esp_err_t ble_err = ble_bridge_init();
        if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE bridge init failed: %s — continuing without BLE",
                 esp_err_to_name(ble_err));
        }

    ESP_LOGI(TAG, "All modules started");
    ESP_LOGI(TAG, "BLE bridge: %s",
             ble_bridge_is_provisioned() ? "provisioned" : "awaiting nRF Mesh app");

       /* Root: send test gossip alert every 5 minutes */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(300000));

        if (esp_mesh_is_root()) {
            uint8_t alert[] = { 0x03 };
            gossip_send(alert, sizeof(alert), GOSSIP_TTL_DEFAULT);
        }

        ESP_LOGI(TAG, "BLE bridge load: %d M5StickC device(s)",
                 ble_bridge_get_load());
        }
    }else{

        ESP_LOGI(TAG,"No WiFi connection detected, skipping BLE bridge initialization");
         ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
            ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
     ESP_ERROR_CHECK(ret);
         esp_err_t er =    node_init();
         if (er != ESP_OK) {
         ESP_LOGE(TAG, "BLE bridge init failed: %s — continuing without BLE",
                 esp_err_to_name(er));
        }


    }

    // while (1) {
    //         vTaskDelay(pdMS_TO_TICKS(100000)); // 30s 

    //         if (esp_mesh_is_root() && esp_mesh_is_device_active()) {
    //             ESP_LOGI(TAG, "Root node sending alert to all children");
    //             uint8_t alert[] = { 0x01 };
    //             gossip_send(alert, sizeof(alert), GOSSIP_TTL_DEFAULT);
    //         }
    //     }


}