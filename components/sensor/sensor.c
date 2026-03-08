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
#include "../mesh/ble_mesh/ble_mesh_init.h"
#include <string.h>

// ============================================
// TEST MODE: Set to 1 for step-by-step testing without mesh
// Set to 0 when ready for mesh networking
// ============================================
#define DHT22_TEST_MODE 1

static uint32_t s_sensor_seq = 0;
static uint8_t  s_my_mac[6]  = {0};
static const char *TAG = "SENSOR";
static bool dht22_initialized = false;

static bool read_dht22_sensor(float *temp, float *hum)
{
    // Read from DHT22 sensor on GPIO26
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
    // TODO: Implement MQ-2 ADC reading on GPIO (TBD)
    // For now, return mock data
    return 18.0f + (esp_random() % 100) * 0.1f; // stub
}

#if DHT22_TEST_MODE
// ============================================
// TEST MODE: Simple sensor reading task
// ============================================
static void test_sensor_reading(void *arg)
{
    (void)arg;
    
    char line1[32];
    char line2[32];
    char line3[32];
    uint32_t reading_count = 0;
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "DHT22 TEST MODE - No mesh networking");
    ESP_LOGI(TAG, "Sensor will read every 10 seconds");
    ESP_LOGI(TAG, "========================================");
    
    // Initial display
    display_clear(COLOR_BLACK);
    display_print(10, 10, "DHT22 Test", COLOR_CYAN, COLOR_BLACK);
    
    vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2s before first reading
    
    while (1) {
        reading_count++;
        
        // Read DHT22 sensor
        float temperature = 0.0f;
        float humidity = 0.0f;
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "========== Reading #%lu ==========", reading_count);
        
        if (!read_dht22_sensor(&temperature, &humidity)) {
            ESP_LOGE(TAG, "DHT22 read FAILED!");
            
            // Display error on screen
            display_clear(COLOR_BLACK);
            display_print(10, 10, "DHT22 Test", COLOR_CYAN, COLOR_BLACK);
            display_print(10, 30, "ERROR!", COLOR_RED, COLOR_BLACK);
            display_print(10, 50, "Check wiring", COLOR_YELLOW, COLOR_BLACK);
            display_print(10, 70, "GPIO26 (G26)", COLOR_YELLOW, COLOR_BLACK);
            
        } else {
            // Success - log to serial
            ESP_LOGI(TAG, "✓ DHT22 read SUCCESS");
            ESP_LOGI(TAG, "  Temperature: %.1f°C", temperature);
            ESP_LOGI(TAG, "  Humidity:    %.1f%%", humidity);
            
            // Display on M5StickC LCD
            display_clear(COLOR_BLACK);
            
            // Title
            display_print(10, 10, "DHT22 Sensor", COLOR_CYAN, COLOR_BLACK);
            
            // Temperature
            snprintf(line1, sizeof(line1), "Temp: %.1fC", temperature);
            display_print(10, 25, line1, COLOR_GREEN, COLOR_BLACK);
            
            // Humidity
            snprintf(line2, sizeof(line2), "Hum:  %.1f%%", humidity);
            display_print(10, 45, line2, COLOR_GREEN, COLOR_BLACK);
            
            // Reading count
            snprintf(line3, sizeof(line3), "Read: %lu", reading_count);
            display_print(10, 65, line3, COLOR_YELLOW, COLOR_BLACK);
        }
        
        ESP_LOGI(TAG, "Next reading in 10 seconds...");
        vTaskDelay(pdMS_TO_TICKS(10000)); // Read every 10 seconds
    }   
}

#else
// ============================================
// MESH MODE: Full mesh networking enabled
// ============================================
static void send_sensor_reading(void *arg)
{
    (void)arg; /* unused parameter */

    // need to check whcih node has sensor

    while (1) {
        // Wait until mesh is up and we have a parent
        if (!esp_mesh_is_device_active()) {
            ESP_LOGW(TAG, "Mesh not connected, waiting...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "Sensor task started");
        
        // Read DHT22 sensor
        float temperature = 25.0f;  // default fallback
        float humidity = 50.0f;     // default fallback
        
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

        // esp-mesh struct
        mesh_data_t data = {
            .data  = (uint8_t *)&pkt,
            .size  = sizeof(pkt),
            .proto = MESH_PROTO_BIN,
            .tos   = MESH_TOS_P2P,
        };
        
        //send to root
        esp_err_t err = esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);

        // ALSO broadcast over BLE mesh to neighbors    
        esp_err_t ble_err = ble_mesh_send_sensor(
            pkt.temperature,
            pkt.humidity,
            pkt.smoke);
        if (ble_err != ESP_OK) {
            ESP_LOGW(TAG, "BLE mesh sensor send failed: %s",
                esp_err_to_name(ble_err));
        }   else {
                ESP_LOGI(TAG, "BLE mesh sensor sent OK");
        }           
        // trigger BLE alert if readings are dangerous  // 
        if (pkt.temperature > 35.0f) {
            char alert[] = "ALERT:HIGH_TEMP";
            ble_mesh_send_alert(alert, strlen(alert));
            ESP_LOGW(TAG, "High temperature alert sent over BLE mesh");
        }
        if (pkt.smoke > 70.0f) {
            char alert[] = "ALERT:HIGH_SMOKE";
            ble_mesh_send_alert(alert, strlen(alert));
            ESP_LOGW(TAG, "High smoke alert sent over BLE mesh");
        }     


        ESP_LOGI(TAG, "I am " MACSTR ", layer %d",
        MAC2STR(s_my_mac),
        esp_mesh_get_layer());

        if (!esp_mesh_is_root()) {
            mesh_addr_t parent;
            esp_err_t parent_err = esp_mesh_get_parent_bssid(&parent);
            ESP_LOGI(TAG, "My parent is " MACSTR "",
                MAC2STR(parent.addr));
            if (parent_err == ESP_OK) {
                // ESP_LOGI(TAG, "Sent sensor data to parent " MACSTR ": %s",
                //         MAC2STR(parent.addr), esp_err_to_name(err));
                routing_record_tx(parent.addr, err == ESP_OK); // true=success, false=fail
            }
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Send failed: %s", esp_err_to_name(err));
        }
        ble_mesh_send_hello(routing_get_etx_to_root(), (uint8_t)esp_mesh_get_layer());
        vTaskDelay(pdMS_TO_TICKS(10000)); // send every 10s
    }

}
#endif

void sensor_init(void)
{
    esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "Sensor init — MAC " MACSTR, MAC2STR(s_my_mac));

    // Initialize DHT22 on GPIO26 (M5StickC G26)
    setDHTgpio(26);
    dht22_initialized = true;
    ESP_LOGI(TAG, "DHT22 initialized on GPIO26");

#if DHT22_TEST_MODE
    ESP_LOGI(TAG, "Starting in TEST MODE (mesh disabled)");
    xTaskCreatePinnedToCore(test_sensor_reading,
                            "dht22_test",
                            4096,
                            NULL,
                            tskIDLE_PRIORITY + 2,
                            NULL,
                            1);
#else
    ESP_LOGI(TAG, "Starting in MESH MODE");
    xTaskCreatePinnedToCore(send_sensor_reading,
                            "sensor_task",
                            4096,
                            NULL,
                            tskIDLE_PRIORITY + 2,
                            NULL,
                            1);
#endif
}