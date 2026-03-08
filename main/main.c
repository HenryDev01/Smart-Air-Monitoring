#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "../components/wifi/wifi_service.h"
#include "../components/mesh/initialization/mesh_init.h"
#include "../components/mesh/routing/mesh_routing.h"
#include "../components/display/display.h"
#include "../components/sensor/sensor.h"
#include "../components/mesh/ble_mesh/ble_mesh_init.h"

static const char *TAG = "m5stick";




void app_main(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Air Quality Mesh Monitor        ║");
    ESP_LOGI(TAG, "║  ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "╚══════════════════════════════════╝");

    /* 1. Display */
    display_init();
    display_clear(COLOR_BLACK);
    display_print(10, 20, "AirMonitor", COLOR_BLUE, COLOR_GREEN);

    // wifi_init_sta();
    // if (wifi_is_connected()) {
    //     printf("WiFi Ready\n");
    // }

    /* 2. Mesh + WiFi — BLE mesh init triggered inside
          mesh_init via MESH_EVENT_PARENT_CONNECTED      */
    ESP_ERROR_CHECK(mesh_init());

    /* 3. ETX Routing */
    routing_init();

    /* 4. Sensor task */
    sensor_init();

    ESP_LOGI(TAG, "All systems started");
}