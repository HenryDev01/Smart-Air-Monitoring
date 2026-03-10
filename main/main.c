#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "../components/wifi/wifi_service.h"
#include "../components/mesh/initialization/mesh_init.h"
#include "../components/mesh/routing/mesh_routing.h"
#include "../components/display/display.h"
#include "../components/sensor/sensor.h"
#include "../components/mesh/flooding/mesh_gossip.h"
#include "../components/mesh/auth/mesh_auth.h"

static const char *TAG = "m5stick";




void app_main(void)
{
    uint8_t self_mac[6];
    char mac_str[32];
    esp_read_mac(self_mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, sizeof(mac_str), "MAC: " MACSTR, MAC2STR(self_mac));


    ESP_LOGI(TAG, "╔══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Air Quality Mesh Monitor        ║");
    ESP_LOGI(TAG, "║  ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "╚══════════════════════════════════╝");

    display_init();
    display_clear(COLOR_BLACK);
    display_print(10, 20, "AirMonitor",  COLOR_BLUE,  COLOR_GREEN);
    display_print(10, 25, mac_str, COLOR_CYAN, COLOR_BLACK);    

    /* 1. Mesh + WiFi */
    ESP_ERROR_CHECK(mesh_init());

    if(esp_mesh_is_root()) {
        display_print(10, 30, "Role: ROOT", COLOR_YELLOW, COLOR_BLACK);
    } else {
        display_print(10, 30, "Role: NODE", COLOR_YELLOW, COLOR_BLACK);
    }
    
    auth_init();
    
    routing_init();

    gossip_init();

    sensor_init();

    // while (1) {
    //         vTaskDelay(pdMS_TO_TICKS(100000)); // 30s 

    //         if (esp_mesh_is_root() && esp_mesh_is_device_active()) {
    //             ESP_LOGI(TAG, "Root node sending alert to all children");
    //             uint8_t alert[] = { 0x01 };
    //             gossip_send(alert, sizeof(alert), GOSSIP_TTL_DEFAULT);
    //         }
    //     }


}