#include "esp_mesh.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "../configuration/air_mesh.h"
#include "../mesh/routing/mesh_routing.h"
#include "../display/display.h"
#include "dht22/DHT22.h"
#include "../mesh/ble_mesh/ble_bridge.h"
#include <string.h>

// Set to 0 to enable Mesh Mode
#define DHT22_TEST_MODE 0

static uint32_t s_sensor_seq = 0;
static uint8_t  s_my_mac[6]  = {0};
static const char *TAG = "SENSOR";
static bool dht22_initialized = false;

static bool read_dht22_sensor(float *temp, float *hum)
{
    int ret = readDHT();
    if (ret != DHT_OK) {
        errorHandler(ret);
        return false;
    }
    *temp = getTemperature();
    *hum = getHumidity();
    return true;
}

static float read_mq2_smoke(void)
{
    return 18.0f + (esp_random() % 100) * 0.1f; 
}

#if DHT22_TEST_MODE
static void test_sensor_reading(void *arg)
{
    (void)arg;
    char line1[32]; char line2[32]; char line3[32];
    uint32_t reading_count = 0;
    
    display_clear(COLOR_BLACK);
    display_print(10, 10, "DHT22 Test", COLOR_CYAN, COLOR_BLACK);
    vTaskDelay(pdMS_TO_TICKS(2000)); 
    
    while (1) {
        reading_count++;
        float temperature = 0.0f;
        float humidity = 0.0f;
        
        if (!read_dht22_sensor(&temperature, &humidity)) {
            ESP_LOGE(TAG, "DHT22 read FAILED!");
            display_clear(COLOR_BLACK);
            display_print(10, 10, "DHT22 Test", COLOR_CYAN, COLOR_BLACK);
            display_print(10, 30, "ERROR!", COLOR_RED, COLOR_BLACK);
        } else {
            ESP_LOGI(TAG, "✓ DHT22 read SUCCESS - Temp: %.1f°C, Hum: %.1f%%", temperature, humidity);
            display_clear(COLOR_BLACK);
            display_print(10, 10, "DHT22 Sensor", COLOR_CYAN, COLOR_BLACK);
            snprintf(line1, sizeof(line1), "Temp: %.1fC", temperature);
            display_print(10, 25, line1, COLOR_GREEN, COLOR_BLACK);
            snprintf(line2, sizeof(line2), "Hum:  %.1f%%", humidity);
            display_print(10, 45, line2, COLOR_GREEN, COLOR_BLACK);
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); 
    }   
}

#else
static void send_sensor_reading(void *arg)
{
    (void)arg; 

    while (1) {
        if (!esp_mesh_is_device_active()) {
            ESP_LOGW(TAG, "Mesh not connected, waiting...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // If this node is the bridge/scanner, skip sending its own sensor data
        if (ble_bridge_is_scanning()) {
            vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
            continue;
        }
        
        float temperature = 25.0f; 
        float humidity = 50.0f;    
        
        if (!read_dht22_sensor(&temperature, &humidity)) {
            ESP_LOGW(TAG, "DHT22 read failed, using fallback values");
        } else {
            ESP_LOGI(TAG, "DHT22: Temp=%.1f°C, Humidity=%.1f%%", temperature, humidity);
        }
        
        pkt_sensor_t pkt = {
            .hdr = {
                .type = PKT_SENSOR_DATA,
                .seq  = s_sensor_seq++,
            },
            .temperature = temperature,
            .humidity    = humidity,
            .smoke       = read_mq2_smoke(),
            .hop_count   = (uint8_t)esp_mesh_get_layer(),
            .etx_to_root = routing_get_etx_to_root(),
        };
        memcpy(pkt.hdr.src_id, s_my_mac, 6);

        mesh_data_t data = {
            .data  = (uint8_t *)&pkt,
            .size  = sizeof(pkt),
            .proto = MESH_PROTO_BIN,
            .tos   = MESH_TOS_P2P,
        };
        
        esp_err_t err = esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);

        //Send the data over BLE Mesh!
        esp_err_t ble_err = ble_advertise_sensor(pkt.temperature, pkt.humidity, pkt.smoke);
        if (ble_err != ESP_OK) {
            ESP_LOGW(TAG, "BLE mesh sensor send failed: %s", esp_err_to_name(ble_err));
        } else {
            ESP_LOGI(TAG, "BLE mesh sensor sent OK");
        }            

        // (Temporarily commented out to ensure compilation)
        // char alert[] = "ALERT";
        // ble_mesh_send_alert(alert, strlen(alert));
        // ble_mesh_send_hello(routing_get_etx_to_root(), (uint8_t)esp_mesh_get_layer());

        vTaskDelay(pdMS_TO_TICKS(10000)); 
    }
}
#endif

void sensor_init(void)
{
    esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "Sensor init — MAC " MACSTR, MAC2STR(s_my_mac));

    setDHTgpio(26);
    dht22_initialized = true;
    ESP_LOGI(TAG, "DHT22 initialized on GPIO26");

#if DHT22_TEST_MODE
    ESP_LOGI(TAG, "Starting in TEST MODE (mesh disabled)");
    xTaskCreatePinnedToCore(test_sensor_reading, "dht22_test", 4096, NULL, tskIDLE_PRIORITY + 2, NULL, 1);
#else
    ESP_LOGI(TAG, "Starting in MESH MODE");
    xTaskCreatePinnedToCore(send_sensor_reading, "sensor_task", 4096, NULL, tskIDLE_PRIORITY + 2, NULL, 1);
#endif
}