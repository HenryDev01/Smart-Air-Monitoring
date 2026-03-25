/*
 * mesh_gossip.c  —  ESP-IDF v5.5
 * ────────────────────────────────
 * Gossip-based flooding with TTL for network-wide alerts and config pushes.
 *
 * Algorithm:
 *   1. Receive packet → check seen-message cache (dedup by msg_id)
 *   2. If new: process payload, decrement TTL
 *   3. If TTL > 0: re-flood after a random jitter (0–50 ms)
 *   4. Jitter uses a short-lived task to avoid blocking the recv task
 */

#include "mesh_gossip.h"
#include "../../sensor/sensor.h"
#include "../../air_mqtt/air_mqtt.h"

#include "esp_mesh.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "GOSSIP";

/* ── seen-message cache (circular buffer) ─────────────────── */
static uint32_t s_seen[GOSSIP_SEEN_SIZE] = {0};
static uint8_t  s_seen_idx = 0;




static bool is_seen(uint32_t id)
{

    for (int i = 0; i < GOSSIP_SEEN_SIZE; i++)
        if (s_seen[i] == id) return true;
    return false;
}

static void mark_seen(uint32_t id)
{
    ESP_LOGI(TAG, "Mark seen msg_id=0x%08lX at idx=%d", id, s_seen_idx);
    s_seen[s_seen_idx] = id;
    s_seen_idx = (s_seen_idx + 1) % GOSSIP_SEEN_SIZE;
}

/* ── jittered re-flood (runs in a short-lived task) ─────────── */
static void refloood_task(void *arg)
{
    pkt_gossip_t *pkt = arg;

    uint32_t jitter = esp_random() % GOSSIP_JITTER_MS;
    vTaskDelay(pdMS_TO_TICKS(jitter));

    mesh_data_t tx = {
        .data  = (uint8_t *)pkt,
        .size  = sizeof(*pkt),
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };
    mesh_addr_t group_id = {{0x01,0x00,0x00,0x00,0x00,0x01}};  
    esp_err_t err = esp_mesh_send(&group_id, &tx, MESH_DATA_GROUP, NULL, 0);
    
    ESP_LOGI(TAG, "Re-flood msg=0x%08lX TTL=%d: %s",
             pkt->msg_id, pkt->ttl, err == ESP_OK ? "ok" : esp_err_to_name(err));

    free(pkt);
    vTaskDelete(NULL);
}

/* ── public: handle incoming packet ──────────────────────── */
void gossip_handle_packet(const pkt_gossip_t *pkt)
{
    if (!pkt) return;

    ESP_LOGW(TAG,"GOSSIP FROM " MACSTR ": msg_id=0x%08lX TTL=%d len=%d",
             MAC2STR(pkt->hdr.src_id), pkt->msg_id, pkt->ttl, pkt->payload_len);

    if (is_seen(pkt->msg_id)) {
        ESP_LOGD(TAG, "Dup msg_id=0x%08lX — drop", pkt->msg_id);
        return;
    }
    mark_seen(pkt->msg_id);

    ESP_LOGI(TAG, "Gossip msg=0x%08lX TTL=%d len=%d",
             pkt->msg_id, pkt->ttl, pkt->payload_len);
    gossip_process_payload(pkt->payload, pkt->payload_len);

    if (pkt->ttl <= 1) return;

    pkt_gossip_t *copy = malloc(sizeof(*copy));
    if (!copy) { ESP_LOGE(TAG, "OOM re-flood"); return; }
    memcpy(copy, pkt, sizeof(*copy));
    copy->ttl--;

    xTaskCreate(refloood_task, "g_flood", 2048, copy, tskIDLE_PRIORITY + 1, NULL);
}

/* ── public: originate a gossip message ──────────────────── */
void gossip_send(const uint8_t *payload, uint8_t len, uint8_t ttl)
{
    if (!payload || len == 0 || len > sizeof(((pkt_gossip_t *)0)->payload)) return;

    static uint32_t seq = 0;
    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);

    pkt_gossip_t pkt = {
        .hdr        = { .type = PKT_GOSSIP, .seq = seq, .origin = SRC_WIFI },
        .msg_id     = (uint32_t)(esp_timer_get_time() >> 10) ^ seq,
        .ttl        = ttl ? ttl : GOSSIP_TTL_DEFAULT,
        .payload_len = len,
    };
    memcpy(pkt.hdr.src_id, my_mac, 6);
    memcpy(pkt.payload, payload, len);
    seq++;

    mark_seen(pkt.msg_id);

    mesh_data_t tx = {
        .data  = (uint8_t *)&pkt,
        .size  = sizeof(pkt),
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };

    
    gossip_process_payload(pkt.payload, pkt.payload_len);

    // esp_err_t err = esp_mesh_send(NULL, &tx, MESH_DATA_P2P, NULL, 0);
     mesh_addr_t group_id = {{0x01,0x00,0x00,0x00,0x00,0x01}};
    esp_err_t err = esp_mesh_send(&group_id, &tx, MESH_DATA_GROUP, NULL, 0);

    ESP_LOGI(TAG, "Gossip originated msg=0x%08lX TTL=%d: %s",
             pkt.msg_id, pkt.ttl, err == ESP_OK ? "ok" : esp_err_to_name(err));
}

/* ── default payload handler (override in main.c) ────────── */
__attribute__((weak))
void gossip_process_payload(const uint8_t *payload, uint8_t len)
{
    ESP_LOGI(TAG, "Gossip payload (%d bytes): %.*s", len, (int)len, payload);
    sensor_cfg_t *cfg = (sensor_cfg_t *)payload;

    if (cfg->type == CFG_TYPE_THRESHOLD) {

        int temp_threshold  = cfg->temp_max;
        int smoke_threshold = cfg->smoke_max;
        sensor_set_thresholds(cfg->temp_max, cfg->smoke_max);


        ESP_LOGI(TAG, "Threshold updated temp=%d smoke=%d",
                 temp_threshold, smoke_threshold);
    }
    else if (cfg->type == CFG_TYPE_ALERT) {
        ESP_LOGW(TAG, "⚠️ Alert from " MACSTR " temp=%.1f smoke=%.1f",
                 MAC2STR(cfg->src_mac), cfg->temp_val, cfg->smoke_val);

        if (esp_mesh_is_root()) {
            mqtt_publish_alert(cfg->src_mac, cfg->temp_val,
                               cfg->smoke_val, cfg->alert_flags);
        }
    }
}

/* ── public: init ─────────────────────────────────────────── */
void gossip_init(void)
{
    memset(s_seen, 0, sizeof(s_seen));
    s_seen_idx = 0;
    ESP_LOGI(TAG, "Gossip init (TTL=%d cache=%d)", GOSSIP_TTL_DEFAULT, GOSSIP_SEEN_SIZE);
}
void gossip_deinit(void)
{
    memset(s_seen, 0, sizeof(s_seen));
    s_seen_idx = 0;
    ESP_LOGI(TAG, "Gossip deinit done");
}