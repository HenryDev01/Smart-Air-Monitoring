#include "utils.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "stdlib.h"       // ← for malloc/free
#include "string.h"     
#include "stdbool.h"

void ap_to_sta_mac(const uint8_t ap[6], uint8_t sta[6])
{
    memcpy(sta, ap, 6);
    sta[5] -= 1;   // decrement last byte
}

void sta_to_ap_mac(const uint8_t sta[6], uint8_t ap[6])
{
    memcpy(ap, sta, 6);
    ap[5] += 1;   // increment last byte
}

bool wifi_signal_strong_enough(void)
{
    char*TAG = "WIFI_SCAN";

// WiFi init/start done by caller — this function just scans
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_wifi_start();

    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_scan_config_t scan_cfg = {
        .ssid        = (uint8_t *)"Galaxy S23 FE 13E4",
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = 100,
            .max = 120,
        },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }

    wifi_ap_record_t *ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    int8_t best_rssi = -127;
    for (int i = 0; i < ap_count; i++) {
        if (strcmp((char *)ap_list[i].ssid, "Galaxy S23 FE 13E4") == 0) {
            if (ap_list[i].rssi > best_rssi)
                best_rssi = ap_list[i].rssi;
        }
    }
    free(ap_list);

    // Always fully deinit so mesh_init can reinit WiFi fresh
    esp_wifi_stop();
    esp_wifi_deinit();

    ESP_LOGI(TAG, "Router '%s' RSSI: %d (threshold: %d)",
             "Galaxy S23 FE 13E4", best_rssi, WIFI_MIN_RSSI_THRESHOLD);
    return best_rssi >= WIFI_MIN_RSSI_THRESHOLD;

}