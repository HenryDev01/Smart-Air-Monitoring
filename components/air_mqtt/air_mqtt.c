#include "air_mqtt.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "../configuration/air_mesh.h"
#include "../mesh/flooding/mesh_gossip.h"
#include <stdio.h>

static const char *TAG = "MQTT";
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected ✅");
            s_connected = true;
            esp_mqtt_client_subscribe(s_client, "mesh/config/all/threshold", 1);
            esp_mqtt_client_subscribe(s_client, "mesh/config/+/threshold", 1);
            break;
        case MQTT_EVENT_DATA: {
            char topic[64]   = {0};
            char payload[128] = {0};
            snprintf(topic,   sizeof(topic),   "%.*s", event->topic_len, event->topic);
            snprintf(payload, sizeof(payload), "%.*s", event->data_len,  event->data);
            ESP_LOGI(TAG, "MQTT received → %s : %s", topic, payload);
            cJSON *json = cJSON_Parse(payload);
            if (!json) {
                ESP_LOGE(TAG, "JSON parse failed");
                break;
            }

            cJSON *temp  = cJSON_GetObjectItem(json, "temp_max");
            cJSON *smoke = cJSON_GetObjectItem(json, "smoke_max");

            if (cJSON_IsNumber(temp) && cJSON_IsNumber(smoke)) {

                int temp_max  = temp->valueint;
                int smoke_max = smoke->valueint;

                ESP_LOGI(TAG, "Config received temp=%d smoke=%d", temp_max, smoke_max);

                send_mesh_config(temp_max, smoke_max);
            }

            cJSON_Delete(json);
            break;
        }  
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            s_connected = false;
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
        default:
            break;
    }
}

void mqtt_publish_sensor(const uint8_t *mac, float temp, float smoke,
                         float etx, uint8_t hops)
{
    if (!s_connected) {
        ESP_LOGW(TAG, "MQTT not connected — dropping sensor data");
        return;
    }

    // topic: mesh/sensor/AABBCCDDEEFF
    char topic[40];
    snprintf(topic, sizeof(topic),
             "mesh/sensor/%02X%02X%02X%02X%02X%02X/data",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // payload: JSON
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"temp\":%.1f,\"smoke\":%.1f,\"etx\":%.2f,\"hops\":%d}",
             temp, smoke, etx, hops);

    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Published → %s : %s", topic, payload);
}
void mqtt_publish_node_status(const node_status_t *info)
{
    if (!s_connected) return;

    char topic[40];
    snprintf(topic, sizeof(topic),
             "mesh/status/%02X%02X%02X%02X%02X%02X",
             info->mac[0], info->mac[1], info->mac[2],
             info->mac[3], info->mac[4], info->mac[5]);

    char parent_str[18];
    snprintf(parent_str, sizeof(parent_str),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             info->parent_mac[0], info->parent_mac[1], info->parent_mac[2],
             info->parent_mac[3], info->parent_mac[4], info->parent_mac[5]);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{"
             "\"status\":\"%s\","
             "\"layer\":%d,"
             "\"parent\":\"%s\","
             "\"rssi\":%d,"
             "\"authenticated\":%s"
             "}",
             info->status,
             info->layer,
             parent_str,
             info->rssi,
             info->authenticated ? "true" : "false");

    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Status → %s : %s", topic, payload);
}


void mqtt_publish_alert(const uint8_t *mac, float temp, float smoke, uint8_t alert)
{
    if (!s_connected) return;

    char topic[48];
    snprintf(topic, sizeof(topic),
             "mesh/alert/%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"temp\":%.1f,\"smoke\":%.1f,\"temp_alert\":%s,\"smoke_alert\":%s}",
             temp, smoke,
             (alert & 0x01) ? "true" : "false",
             (alert & 0x02) ? "true" : "false");

    // retain=0 — alerts should not persist
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    ESP_LOGW("MQTT", "Alert published → %s : %s", topic, payload);
}

void send_mesh_config(uint8_t temp, uint8_t smoke)
{
    sensor_cfg_t cfg = {
        .type = CFG_TYPE_THRESHOLD,
        .temp_max = temp,
        .smoke_max = smoke
    };

    gossip_send((uint8_t *)&cfg, sizeof(cfg), GOSSIP_TTL_DEFAULT);
}

esp_err_t mqtt_init(void)
{
        esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = "esp32-root-01",
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
    };

    
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) return ESP_FAIL;

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    
    return esp_mqtt_client_start(s_client);
}

void mqtt_deinit(void)
{
    if (s_client == NULL) return;

    esp_mqtt_client_stop(s_client);      // ← stops reconnect task
    esp_mqtt_client_destroy(s_client);   // ← frees all resources
    s_client    = NULL;
    s_connected = false;
    ESP_LOGI(TAG, "MQTT client destroyed");
}