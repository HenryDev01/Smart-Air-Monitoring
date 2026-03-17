#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Must match m5stick_unified firmware exactly ─────────── */
#define CID_ESP                 0x02E5
#define VENDOR_MODEL_ID         0x0000
#define OP_SENSOR_DATA   ESP_BLE_MESH_MODEL_OP_3(0x00, CID_ESP)
#define OP_SENSOR_ACK    ESP_BLE_MESH_MODEL_OP_3(0x01, CID_ESP)
#define OP_BRIDGE_ADVERT ESP_BLE_MESH_MODEL_OP_3(0x02, CID_ESP)
#define OP_BRIDGE_SELECT ESP_BLE_MESH_MODEL_OP_3(0x03, CID_ESP)
#define GROUP_SENSOR        0xC000
#define GROUP_BRIDGE_ADVERT 0xC001
#define BRIDGE_ADVERT_MS  15000   // was 5000

/*
Tempaory hardcoded key for provisioning
*/
#define PROV_NET_IDX   0x0000
#define PROV_APP_IDX   0x0000
#define PROV_NET_KEY   { 0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF, \
                         0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF }
#define PROV_APP_KEY   { 0xFE,0xDC,0xBA,0x98,0x76,0x54,0x32,0x10, \
                         0xFE,0xDC,0xBA,0x98,0x76,0x54,0x32,0x10 }


//Payload/Packet
typedef struct __attribute__((packed)) {
    uint8_t src_mac[6];
    float   temperature;
    float   humidity;
    float   smoke;
    uint8_t battery_pct;
} ble_sensor_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t bridge_mac[6];
    uint8_t wifi_connected;
    uint8_t wifi_layer;
    uint8_t load;
    int8_t  rssi_to_root;
} ble_bridge_advert_t;

typedef struct __attribute__((packed)) {
    uint8_t sensor_mac[6];
    uint8_t selected;
} ble_bridge_select_t;

