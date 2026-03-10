#include "mesh_routing.h"

#include "esp_mesh.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "ROUTING";

/* ── state ────────────────────────────────────────────────── */
static neighbor_t       s_nb[NEIGHBOR_MAX];
static SemaphoreHandle_t s_nb_mutex;
static uint32_t         s_hello_seq   = 0;
static float            s_etx_to_root = ETX_INFINITY;
static uint8_t          s_my_mac[6]   = {0};



static inline uint32_t now_ms(void)
{
    /* esp_timer_get_time() → int64_t µs; divide to ms and cast to uint32_t */
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static float compute_etx(const neighbor_t *n)
{   ESP_LOGI(TAG, "Computing ETX for neighbor " MACSTR ": tx_count=%u ack_count=%u",
             MAC2STR(n->mac), n->tx_count, n->ack_count);

    if (n->tx_count == 0) return ETX_INFINITY;
    float pdr = (float)n->ack_count / (float)n->tx_count; // ack to packet sent ratio
    return (pdr > 0.01f) ? (1.0f / pdr) : ETX_INFINITY;
}

/* neighbor function */

// find source neighbor table by MAC address 
static neighbor_t *find_nb(const uint8_t *mac)
{
    if(!esp_mesh_is_root())
    {
              ESP_LOGI(TAG, "Find Neighbor " MACSTR "",
                 MAC2STR(mac));

                  for (int i = 0; i < NEIGHBOR_MAX; i++)
                  {
                     ESP_LOGI(TAG, "Neighbor[%d]: valid=%d " MACSTR, i, s_nb[i].valid, MAC2STR(s_nb[i].mac));

                  }
    }
  
    
    for (int i = 0; i < NEIGHBOR_MAX; i++)
        if (s_nb[i].valid && memcmp(s_nb[i].mac, mac, 6) == 0)
            return &s_nb[i];
    return NULL;
}



/*

if table has empty slots then insert into it. slot 2 is empty so mac inserted
| Slot | Valid | MAC    |  etx
| ---- | ----- | -----  |  ----
| 0    | ✅     | A     |  1
| 1    | ✅     | B     |  2
| 2    | ❌     | empty |
| 3    | ❌     | empty |

if table full, then replace node with the worst etx.
*/
static neighbor_t *alloc_nb(void)
{
    /* Free slot first */
    for (int i = 0; i < NEIGHBOR_MAX; i++)
        if (!s_nb[i].valid) return &s_nb[i];

    /* Evict worst (highest ETX) */
    neighbor_t *worst = &s_nb[0];
    for (int i = 1; i < NEIGHBOR_MAX; i++)
        if (s_nb[i].etx > worst->etx) worst = &s_nb[i];
    memset(worst, 0, sizeof(*worst));
    return worst;
}

// count transmission record and compute the etx to the destination
void routing_record_tx(const uint8_t *mac, bool acked)
{
    if (!mac) return;
    xSemaphoreTake(s_nb_mutex, portMAX_DELAY);
    neighbor_t *n = find_nb(mac);
    if (n) {
        
        if (n->tx_count >= ETX_WINDOW_SIZE) {
            /* Decay sliding window */
            n->tx_count  = ETX_WINDOW_SIZE / 2;
            n->ack_count = (uint32_t)(n->ack_count * 0.5f);
        }
        n->tx_count++;
        if (acked) n->ack_count++;
        n->etx = compute_etx(n);
        ESP_LOGI(TAG, "Updated neighbor " MACSTR ": ETX=%.2f",
                 MAC2STR(n->mac), n->etx);

    }
    else{
        ESP_LOGW(TAG, "No neighbor entry for " MACSTR, MAC2STR(mac));
    }
     xSemaphoreGive(s_nb_mutex);
    
}

/* ── public: handle incoming HELLO ───────────────────────── */
void routing_handle_hello(const pkt_hello_t *pkt, const mesh_addr_t *from)
{
    if (!pkt || !from) return;

    xSemaphoreTake(s_nb_mutex, portMAX_DELAY);

    neighbor_t *n = find_nb(pkt->hdr.src_id);
    if (!n) {
        n = alloc_nb();
        memcpy(n->mac, pkt->hdr.src_id, 6);  // ← same key
        n->etx = ETX_INFINITY;
        ESP_LOGI(TAG, "New neighbor " MACSTR, MAC2STR(from->addr));
    }
    n->valid         = true;
    n->etx_to_root   = pkt->etx_to_root;
    n->rssi          = pkt->rssi;
    n->last_hello_ms = now_ms();

    xSemaphoreGive(s_nb_mutex);

    routing_update_parent();
}

/* ── public: re-evaluate best parent ─────────────────────── */
void routing_update_parent(void)
{
    if (esp_mesh_is_root()) {
        ESP_LOGI(TAG, "I AM ROOT I WILL NOT CALCULATE THE ETX");
        s_etx_to_root = 0.0f;
        return;
    }

    xSemaphoreTake(s_nb_mutex, portMAX_DELAY);

    float   best_etx = ETX_INFINITY;
    uint8_t best_mac[6] = {0};
    bool    found = false;
    uint32_t t = now_ms();

    for (int i = 0; i < NEIGHBOR_MAX; i++) {
        neighbor_t *n = &s_nb[i];
        if (!n->valid) continue;
        /* Prune stale */
        if ((t - n->last_hello_ms) > NEIGHBOR_TIMEOUT_MS) { // if neighbor is not heard from for a long time. remve it.
            ESP_LOGD(TAG, "Pruned stale " MACSTR, MAC2STR(n->mac));
            n->valid = false;
            continue;
        }
        float path = n->etx + n->etx_to_root;
        if (path < best_etx) {
            best_etx = path;
            memcpy(best_mac, n->mac, 6);
            found = true;
        }
    }

    xSemaphoreGive(s_nb_mutex);

    if (found && best_etx < s_etx_to_root * ETX_SWITCH_HYSTERESIS) {
        ESP_LOGI(TAG, "New parent " MACSTR " ETX %.2f→%.2f",
                 MAC2STR(best_mac), s_etx_to_root, best_etx);

        /*Current:  Node C → Parent B
        After:    Node C → Parent A  (better ETX path)*/
        wifi_config_t parent_cfg = {0};
        memcpy(parent_cfg.sta.bssid, best_mac, 6);
        parent_cfg.sta.bssid_set = true;
        mesh_addr_t mesh_id = {0};
        esp_mesh_set_parent(&parent_cfg, &mesh_id, MESH_NODE, esp_mesh_get_layer());
        s_etx_to_root = best_etx;
    }
}

void routing_invalidate_parent(void) { s_etx_to_root = ETX_INFINITY; }
void routing_reset_etx(void)         { s_etx_to_root = esp_mesh_is_root() ? 0.0f : 1.0f; }
float routing_get_etx_to_root(void)  { return s_etx_to_root; }

/* ── HELLO broadcast task ─────────────────────────────────── */
static void hello_task(void *arg)
{
    ESP_LOGI(TAG, "Hello task started");
    while (1) {
        pkt_hello_t h = {
            .hdr = { .type = PKT_HELLO, .seq = s_hello_seq++ },
            .etx_to_root    = s_etx_to_root,
            .hop_count      = (uint8_t)esp_mesh_get_layer(),
            .layer          = (uint8_t)esp_mesh_get_layer(),
            .rssi           = 0,   /* optionally fill from wifi_ap_record_t */
        };
        memcpy(h.hdr.src_id, s_my_mac, 6);
            ESP_LOGI(TAG, "testing — MAC " MACSTR, MAC2STR(h.hdr.src_id));


        mesh_data_t tx = {
            .data  = (uint8_t *)&h,
            .size  = sizeof(h),
            .proto = MESH_PROTO_BIN,
            .tos   = MESH_TOS_P2P,
        };
        /* NULL dest and MESH_DATA_P2P = broadcast to all immediate neighbors */
        //esp_mesh_send(NULL, &tx, MESH_DATA_P2P, NULL, 0);
        // if (esp_mesh_is_root()) {
        //     // Root sends HELLO down to each connected child
        //     // esp-mesh internal routing table
        //     mesh_addr_t route_table[NEIGHBOR_MAX];
        //     int route_table_size = 0;
        //     esp_mesh_get_routing_table(route_table,
        //                                NEIGHBOR_MAX * sizeof(mesh_addr_t),
        //                                &route_table_size);

        //     for (int i = 0; i < route_table_size; i++) {
        //         esp_err_t err = esp_mesh_send(&route_table[i], &tx,
        //                                       MESH_DATA_P2P, NULL, 0);
        //         ESP_LOGI(TAG, "Root HELLO → " MACSTR ": %s",
        //                  MAC2STR(route_table[i].addr), esp_err_to_name(err));
        //     }
        // } else {
        //     // Child sends HELLO up to parent
        //     mesh_addr_t parent;
        //     if (esp_mesh_get_parent_bssid(&parent) == ESP_OK) {
        //         esp_err_t err = esp_mesh_send(&parent, &tx,
        //                                       MESH_DATA_P2P, NULL, 0);
        //         ESP_LOGI(TAG, "Child HELLO → parent " MACSTR ": %s",
        //                  MAC2STR(parent.addr), esp_err_to_name(err));
        //     }
        // }

        // broadcast doesnt seem to work, had to put nodes into group and multicast it for child to receive data.
        mesh_addr_t group_id = {{0x01,0x00,0x00,0x00,0x00,0x01}};
        esp_mesh_send(&group_id, &tx, MESH_DATA_GROUP, NULL, 0);


        

        vTaskDelay(pdMS_TO_TICKS(HELLO_INTERVAL_MS));
    }
}
/* ── public: init ─────────────────────────────────────────── */
void routing_init(void)
{
    memset(s_nb, 0, sizeof(s_nb));
    s_nb_mutex = xSemaphoreCreateMutex();

    /* IDF 5.5: use esp_read_mac() for STA MAC */
    esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);  // ← changed from ESP_MAC_WIFI_STA    
    routing_reset_etx();

    /* pinned to core 1, low priority — background task */
    xTaskCreatePinnedToCore(hello_task, "hello_bcast", 2560, NULL,
                            tskIDLE_PRIORITY + 2, NULL, 1);

    ESP_LOGI(TAG, "Routing init — MAC " MACSTR, MAC2STR(s_my_mac));
}