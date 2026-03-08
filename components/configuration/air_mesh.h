#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_mesh.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_ble_mesh_defs.h"
#include "secrets.h"

/* ─────────────────────────────────────────────
   MESH CONFIGURATION
   ───────────────────────────────────────────── */
#define MESH_ID              {0xAB, 0x72, 0x4D, 0x6F, 0x6E, 0x00}  // "AirMon"
#define MESH_CHANNEL         6
#define MESH_MAX_LAYERS      6
#define MESH_AP_MAX_CONN     6


/* ─────────────────────────────────────────────
   ETX / ROUTING CONFIGURATION
   ───────────────────────────────────────────── */
#define NEIGHBOR_MAX         10
#define ETX_WINDOW_SIZE      20        // packets tracked per neighbor
#define ETX_INFINITY         999.0f
#define ETX_SWITCH_HYSTERESIS 0.85f   /* only switch parent if >15% better */
#define HELLO_INTERVAL_MS    10000     // 10 seconds
#define NEIGHBOR_TIMEOUT_MS  35000     // 3.5× hello interval → stale

/* ─────────────────────────────────────────────
   GOSSIP CONFIGURATION
   ───────────────────────────────────────────── */
#define GOSSIP_TTL_DEFAULT   6
#define GOSSIP_SEEN_SIZE     32
#define GOSSIP_JITTER_MS     50        // random delay to reduce collision

/* ─────────────────────────────────────────────
   SECURITY CONFIGURATION
   ───────────────────────────────────────────── */
#define HMAC_KEY_LEN         32
#define NONCE_LEN            16
#define SESSION_TTL_SEC      3600      // 1 hour
#define TIMESTAMP_TOLERANCE  30        // seconds replay protection window
#define AUTH_RETRY_MAX       3

/* ─────────────────────────────────────────────
   ENERGY CONFIGURATION
   ───────────────────────────────────────────── */
#define SENSOR_INTERVAL_MS   30000     // 30s between readings
#define CPU_MAX_FREQ_MHZ     80
#define CPU_MIN_FREQ_MHZ     10

/* ─────────────────────────────────────────────
   BLE MESH CONFIGURATION
   ───────────────────────────────────────────── */
#define BLE_MESH_COMPANY_ID        0x02E5   // Espressif
#define BLE_MESH_PRODUCT_ID        0x0001
#define BLE_MESH_VERSION_ID        0x0001

#define BLE_MESH_APP_KEY_IDX       0x0000
#define BLE_MESH_NET_KEY_IDX       0x0000
#define BLE_MESH_GROUP_ADDR        0xC000   // multicast group for all nodes

#define BLE_MESH_SENSOR_MODEL_ID   0x1100   // vendor model: sensor data
#define BLE_MESH_ALERT_MODEL_ID    0x1101   // vendor model: alerts/gossip
#define BLE_MESH_HELLO_MODEL_ID    0x1102   // vendor model: node discovery

#define AIR_MESH_TTL_DEFAULT       7
#define BLE_MESH_SEND_TIMEOUT_MS   3000

/* ─────────────────────────────────────────────
   BLE MESH OPCODES (Vendor Model)
   ───────────────────────────────────────────── */
#define BLE_MESH_VND_OP_SENSOR     ESP_BLE_MESH_MODEL_OP_3(0x00, BLE_MESH_COMPANY_ID)
#define BLE_MESH_VND_OP_ALERT      ESP_BLE_MESH_MODEL_OP_3(0x01, BLE_MESH_COMPANY_ID)
#define BLE_MESH_VND_OP_HELLO      ESP_BLE_MESH_MODEL_OP_3(0x02, BLE_MESH_COMPANY_ID)
#define BLE_MESH_VND_OP_SENSOR_ACK ESP_BLE_MESH_MODEL_OP_3(0x03, BLE_MESH_COMPANY_ID)

/* ─────────────────────────────────────────────
   PACKET TYPES
   ───────────────────────────────────────────── */
typedef enum {
    PKT_SENSOR_DATA    = 0x01,
    PKT_HELLO          = 0x02,
    PKT_GOSSIP         = 0x03,
    PKT_JOIN_REQUEST   = 0x10,
    PKT_JOIN_RESPONSE  = 0x11,
    PKT_CHALLENGE      = 0x12,
    PKT_CHALLENGE_RESP = 0x13,
    PKT_SESSION_REVOKE = 0x14,
} pkt_type_t;

/* ─────────────────────────────────────────────
   PACKET STRUCTURES
   ───────────────────────────────────────────── */

// Generic header on every packet
typedef struct __attribute__((packed)) {
    uint8_t    type;           // pkt_type_t
    uint8_t    src_id[6];      // sender MAC
    uint32_t   seq;            // monotonic sequence number
} pkt_hdr_t;

// Sensor reading (node → root)
typedef struct __attribute__((packed)) {
    pkt_hdr_t  hdr;
    float      temperature;
    float      humidity; 
    float      smoke;       
    uint8_t    hop_count;
    float      etx_to_root;
} pkt_sensor_t;

// Proactive hello (broadcast to neighbors)
typedef struct __attribute__((packed)) {
    pkt_hdr_t  hdr;
    float      etx_to_root;
    uint8_t    hop_count;
    uint8_t    layer;
    int8_t     rssi;           // RSSI to own parent
} pkt_hello_t;

// Gossip flood packet (alerts, config)
typedef struct __attribute__((packed)) {
    pkt_hdr_t  hdr;
    uint32_t   msg_id;         // unique message id for dedup
    uint8_t    ttl;
    uint8_t    payload[64];
    uint8_t    payload_len;
} pkt_gossip_t;

// Join request (new node → root)
typedef struct __attribute__((packed)) {
    pkt_hdr_t  hdr;
    uint8_t    nonce[NONCE_LEN];
    uint32_t   timestamp;
    uint8_t    hmac[32];       // HMAC-SHA256(node_id || nonce || timestamp, key)
} pkt_join_req_t;

// Join response (root → new node)
typedef struct __attribute__((packed)) {
    pkt_hdr_t  hdr;
    uint8_t    accepted;       // 1 = ok, 0 = rejected
    uint8_t    challenge[NONCE_LEN];
    uint32_t   session_expires;
} pkt_join_resp_t;

// Challenge-response (periodic re-auth)
typedef struct __attribute__((packed)) {
    pkt_hdr_t  hdr;
    uint8_t    challenge[NONCE_LEN];
} pkt_challenge_t;

typedef struct __attribute__((packed)) {
    pkt_hdr_t  hdr;
    uint8_t    response[32];   // HMAC-SHA256(challenge, key)
} pkt_challenge_resp_t;

/* ─────────────────────────────────────────────
   NEIGHBOR TABLE ENTRY
   ───────────────────────────────────────────── */
typedef struct {
    uint8_t    mac[6];
    float      etx;                    // computed ETX to this neighbor
    float      etx_to_root;            // neighbor's reported ETX to root
    uint32_t   tx_count;               // packets sent to this neighbor
    uint32_t   ack_count;              // ACKs received from this neighbor
    int8_t     rssi;
    uint32_t   last_hello_ms;          // esp_log_timestamp() of last hello
    bool       valid;
} neighbor_t;

/* ─────────────────────────────────────────────
   SESSION TABLE ENTRY
   ───────────────────────────────────────────── */
typedef struct {
    uint8_t    node_id[6];
    uint8_t    challenge[NONCE_LEN];   // pending challenge issued by root
    uint32_t   issued_at;              // unix timestamp
    uint32_t   session_expires;        // unix timestamp
    bool       authenticated;
    uint8_t    fail_count;
} session_t;