/*
 * mesh_auth.c  —  ESP-IDF v5.5
 * ─────────────────────────────
 * HMAC-SHA256 node authentication + 1-hour session management.
 * 
 * 
 * */
#include "mesh_auth.h"

#include "esp_mesh.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/md.h"

/* ── own credentials ──────────────────────────────────────── */
static uint8_t s_my_mac[6]        = {0};
static uint8_t s_my_key[HMAC_KEY_LEN] = {0};
static bool    s_key_loaded        = false;

/* ── client-side session state ────────────────────────────── */
static bool    s_authenticated     = false;
static uint32_t s_session_expires  = 0;

/* ── server-side session table (root only) ────────────────── */
#define SESSION_MAX 20
static session_t        s_sessions[SESSION_MAX];
static SemaphoreHandle_t s_sess_mutex;

/* ── Attack simulation flags (set to 1 to enable) ────────── */
#define SIM_REPLAY_ATTACK    0   /* force timestamp to be stale        */
#define SIM_WRONG_HMAC       0   /* corrupt the HMAC before verify     */
#define SIM_SPOOF_MAC        0   /* replace src_id with fake MAC       */
#define SIM_UNKNOWN_NODE     0   /* pretend node key doesn't exist     */

#define MAX_AUTH_RETRIES  3
static int s_auth_retries = 0;

static const char *TAG = "AUTH";

static auth_node_authenticated_cb_t s_auth_cb = NULL;
static TaskHandle_t s_watchdog_handle = NULL;


void auth_set_authenticated_cb(auth_node_authenticated_cb_t cb)
{
    s_auth_cb = cb;
}

//hmac

static esp_err_t do_hmac(const uint8_t *key, size_t klen,
                          const uint8_t *msg, size_t mlen,
                          uint8_t out[32])
{
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_setup(&ctx, info, 1) != 0) {
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }
    mbedtls_md_hmac_starts(&ctx, key, klen);
    mbedtls_md_hmac_update(&ctx, msg, mlen);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    return ESP_OK;
}

// replay attack prevention
// 0xAA ^ 0xAA = 0
// 0xAA ^ 0xAB = 1
static bool hmac_eq(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

/* Build canonical HMAC message: src_id(6) || nonce(16) || timestamp(4) */
static void build_join_msg(const uint8_t *src_id, const uint8_t *nonce,
                            uint32_t ts, uint8_t out[26])
{
    memcpy(out,      src_id, 6);
    memcpy(out + 6,  nonce,  NONCE_LEN);
    memcpy(out + 22, &ts,    4);
}

// NVS Key
esp_err_t auth_load_credentials(void)
{
    esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);

    nvs_handle_t h;
    if (nvs_open("node_creds", NVS_READONLY, &h) != ESP_OK) {
        /* Development fallback — REPLACE IN PRODUCTION */
        ESP_LOGW(TAG, "No NVS creds — using test key (NOT SECURE)");
        memset(s_my_key, 0xAB, HMAC_KEY_LEN);
        s_key_loaded = true;
        return ESP_OK;
    }
    size_t len = HMAC_KEY_LEN;
    esp_err_t err = nvs_get_blob(h, "hmac_key", s_my_key, &len);
    nvs_close(h);
    if (err == ESP_OK && len == HMAC_KEY_LEN) {
        s_key_loaded = true;
        ESP_LOGI(TAG, "Key loaded for " MACSTR, MAC2STR(s_my_mac));
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t auth_provision_key(const uint8_t *key, size_t len)
{
    if (len != HMAC_KEY_LEN) return ESP_ERR_INVALID_ARG;

    //Namespace: node_creds
    //Key:       hmac_key
    //Value:     <32 byte HMAC key>
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("node_creds", NVS_READWRITE, &h)); //open up the nvs storage "node_creds" spcae space
    esp_err_t err = nvs_set_blob(h, "hmac_key", key, len);

    nvs_commit(h);
    nvs_close(h);

    return err;
}

/* Lookup another node's shared key (root only).
 * Key is stored under MAC-as-string in NVS namespace "node_keys".
 * Falls back to global test key during development. */
static bool lookup_peer_key(const uint8_t *mac, uint8_t key_out[HMAC_KEY_LEN])
{
    char ns[20];
    snprintf(ns, sizeof(ns), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    nvs_handle_t h;
    if (nvs_open("node_keys", NVS_READONLY, &h) != ESP_OK) {
        memset(key_out, 0xAB, HMAC_KEY_LEN); /* dev fallback */
        return true;
    }
    size_t len = HMAC_KEY_LEN;
    bool ok = (nvs_get_blob(h, ns, key_out, &len) == ESP_OK &&
               len == HMAC_KEY_LEN);
    nvs_close(h);
    return ok;
}

// fix — find by MAC regardless of state, as long as node_id is set
static session_t *sess_find(const uint8_t *mac)
{
    for (int i = 0; i < SESSION_MAX; i++)
        if (memcmp(s_sessions[i].node_id, mac, 6) == 0 &&
            (s_sessions[i].authenticated     ||
             s_sessions[i].fail_count > 0    ||
             s_sessions[i].issued_at > 0))      // ← pending session check
            return &s_sessions[i];
    return NULL;
}

static session_t *sess_alloc(const uint8_t *mac)
{
    for (int i = 0; i < SESSION_MAX; i++) {
        if (!s_sessions[i].authenticated && s_sessions[i].fail_count == 0) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            memcpy(s_sessions[i].node_id, mac, 6);
            return &s_sessions[i];
        }
    }
    return NULL; /* table full */
}


static inline uint32_t unix_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}
// client node

// this method basically initiates the join process by sending a join request to the root node. 
//It constructs a join request packet, fills in the necessary fields 
// (including generating a random nonce and timestamp), computes the HMAC for authentication, 
// and sends the packet to the root node using ESP-MESH.
void auth_send_join_request(void)
{
     if (!s_key_loaded) {
        ESP_LOGE(TAG, "No credentials loaded");
        return;
    }

    pkt_join_req_t req = { .hdr = { .type = PKT_JOIN_REQUEST } };
    // generate messages for hmac: src_id(6) || nonce(16) || timestamp(4)
    memcpy(req.hdr.src_id, s_my_mac, 6); //copy source mac address to packet header src id
    esp_fill_random(req.nonce, NONCE_LEN); // random nonce for replay attack prevention
    req.timestamp = unix_now(); // time stamp

    uint8_t msg[26];
    build_join_msg(s_my_mac, req.nonce, req.timestamp, msg); // copying  mac, nonce and timestamp bytes to msg (26 bytes)
    do_hmac(s_my_key, HMAC_KEY_LEN, msg, sizeof(msg), req.hmac); // generate 32 byte hmac and store in req.hmac based on msg and key

    mesh_data_t tx = { .data  = (uint8_t *)&req, .size  = sizeof(req),
                       .proto = MESH_PROTO_BIN,   .tos   = MESH_TOS_P2P };
    esp_err_t err = esp_mesh_send(NULL, &tx, MESH_DATA_TODS, NULL, 0);
    ESP_LOGI(TAG, "Join request → root: %s", esp_err_to_name(err));

}

void auth_handle_join_response(const pkt_join_resp_t *pkt)
{
    if (pkt->accepted) {
        ESP_LOGI(TAG, "Join accepted — responding to challenge");
        s_session_expires = pkt->session_expires;
        // /* Respond to embedded challenge */
        pkt_challenge_t fake = { .hdr = { .type = PKT_CHALLENGE } };
        memcpy(fake.challenge, pkt->challenge, NONCE_LEN);
        auth_handle_challenge(&fake);
    } else {
        ESP_LOGW(TAG, "Join rejected by root");
    }
}

void auth_handle_challenge(const pkt_challenge_t *pkt)
{
    pkt_challenge_resp_t resp = { .hdr = { .type = PKT_CHALLENGE_RESP } };
    memcpy(resp.hdr.src_id, s_my_mac, 6);
    do_hmac(s_my_key, HMAC_KEY_LEN, pkt->challenge, NONCE_LEN, resp.response);

    mesh_data_t tx = { .data  = (uint8_t *)&resp, .size  = sizeof(resp),
                       .proto = MESH_PROTO_BIN,    .tos   = MESH_TOS_P2P };
    esp_err_t err = esp_mesh_send(NULL, &tx, MESH_DATA_TODS, NULL, 0);
    ESP_LOGI(TAG, "Challenge response sent: %s", esp_err_to_name(err));
}

// root

void auth_handle_join_request(const pkt_join_req_t *req,
                               const mesh_addr_t *from)
{
    ESP_LOGI(TAG, "Join from " MACSTR, MAC2STR(from->addr));

    /* ── make a mutable copy so we can tamper with it ──────── */
    pkt_join_req_t r = *req;

#if SIM_REPLAY_ATTACK
    r.timestamp = unix_now() - (TIMESTAMP_TOLERANCE + 60);
    ESP_LOGW(TAG, "[SIM] Replay attack — timestamp forced stale: %lu", r.timestamp);
#endif

#if SIM_SPOOF_MAC
    static const uint8_t fake_mac[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };
    memcpy(r.hdr.src_id, fake_mac, 6);
    ESP_LOGW(TAG, "[SIM] MAC spoof — src_id replaced with DE:AD:BE:EF:00:01");
#endif

#if SIM_WRONG_HMAC
    memset(r.hmac, 0x00, 32);
    ESP_LOGW(TAG, "[SIM] Wrong HMAC — zeroed out HMAC bytes");
#endif


    /* 1. Replay protection */
    // need to modify this.
    // Right now unix_now() is just seconds since boot, not real time. 
    // If nodes reboot at different times, timestamps will be out of sync,
    uint32_t now = unix_now();
    if ((int32_t)(now - r.timestamp) > TIMESTAMP_TOLERANCE ||
        (int32_t)(now - r.timestamp) < -TIMESTAMP_TOLERANCE) {
        ESP_LOGW(TAG, "Timestamp out of window — rejecting");
        goto reject;
    }

    /* 2. Key lookup */
    uint8_t key[HMAC_KEY_LEN];

#if SIM_UNKNOWN_NODE
    static const uint8_t unknown_mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    ESP_LOGW(TAG, "[SIM] Unknown node — looking up fake MAC instead");
    if (!lookup_peer_key(unknown_mac, key)) {
#else
    if (!lookup_peer_key(from->addr, key)) {
#endif
        ESP_LOGW(TAG, "Unknown node " MACSTR, MAC2STR(from->addr));
        goto reject;
    }

    /* 3. Verify HMAC */
    uint8_t msg[26], expected[32];
    build_join_msg(r.hdr.src_id, r.nonce, r.timestamp, msg);
    do_hmac(key, HMAC_KEY_LEN, msg, sizeof(msg), expected);

    if (!hmac_eq(expected, r.hmac)) {
        ESP_LOGW(TAG, "HMAC mismatch from " MACSTR, MAC2STR(from->addr));
        xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
        session_t *s = sess_find(from->addr);
        if (s) {s->fail_count++;}

        // if (s_auth_retries++ >= MAX_AUTH_RETRIES) {
        //     xSemaphoreGive(s_sess_mutex);
        //     kick_node(from);    // only kick after 3 failures
        //     goto reject;
        // }
        xSemaphoreGive(s_sess_mutex);
        goto reject;
    }

    /* 4. Accept — create session and issue challenge */
    xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
    session_t *sess = sess_find(from->addr);
    if (!sess) sess = sess_alloc(from->addr);
    if (sess) {
        esp_fill_random(sess->challenge, NONCE_LEN);
        sess->issued_at      = now;
        sess->session_expires = now + SESSION_TTL_SEC;
        sess->fail_count     = 0;
        sess->authenticated  = false; /* pending challenge response */
        ESP_LOGI(TAG, "Session created for " MACSTR, MAC2STR(from->addr));
    }
    xSemaphoreGive(s_sess_mutex);

    pkt_join_resp_t resp = { .hdr = { .type = PKT_JOIN_RESPONSE }, .accepted = 1 };
    if (sess) {
        memcpy(resp.challenge, sess->challenge, NONCE_LEN);
        resp.session_expires = sess->session_expires;
    }

    mesh_data_t tx = { .data  = (uint8_t *)&resp, .size  = sizeof(resp),
                       .proto = MESH_PROTO_BIN,    .tos   = MESH_TOS_P2P };
    esp_mesh_send((mesh_addr_t *)from, &tx, MESH_DATA_FROMDS, NULL, 0);
    ESP_LOGI(TAG, "Join accepted + challenge → " MACSTR, MAC2STR(from->addr));
    return;

    reject: {
        pkt_join_resp_t rej = { .hdr = { .type = PKT_JOIN_RESPONSE }, .accepted = 0 };
        mesh_data_t tx = { .data  = (uint8_t *)&rej, .size  = sizeof(rej),
                        .proto = MESH_PROTO_BIN,   .tos   = MESH_TOS_P2P };
        esp_mesh_send((mesh_addr_t *)from, &tx, MESH_DATA_FROMDS, NULL, 0);
    }
}

void auth_handle_challenge_response(const pkt_challenge_resp_t *pkt,
                                     const mesh_addr_t *from)
{
    xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
    session_t *sess = sess_find(from->addr);
    if (!sess) {
        xSemaphoreGive(s_sess_mutex);
        ESP_LOGW(TAG, "Response from unknown node " MACSTR, MAC2STR(from->addr));
        return;
    }

    uint8_t key[HMAC_KEY_LEN], expected[32];
    lookup_peer_key(from->addr, key);
    do_hmac(key, HMAC_KEY_LEN, sess->challenge, NONCE_LEN, expected);

    if (hmac_eq(expected, pkt->response)) {
        sess->authenticated = true;
        sess->fail_count    = 0;
        ESP_LOGI(TAG, "Node " MACSTR " AUTHENTICATED ✓", MAC2STR(from->addr));
    //if (s_auth_cb) s_auth_cb(from->addr);  // ← fire callback

    } else {
        sess->fail_count++;
        ESP_LOGW(TAG, "Challenge fail %d/%d for " MACSTR,
                 sess->fail_count, AUTH_RETRY_MAX, MAC2STR(from->addr));
        if (sess->fail_count >= AUTH_RETRY_MAX) {
            uint8_t mac[6];
            memcpy(mac, from->addr, 6);
            xSemaphoreGive(s_sess_mutex);
            auth_revoke_node(mac);
            return;
        }
    }
    xSemaphoreGive(s_sess_mutex);
}

void auth_send_challenge(const uint8_t *mac)
{
    pkt_challenge_t chal = { .hdr = { .type = PKT_CHALLENGE } };
    memcpy(chal.hdr.src_id, s_my_mac, 6);
    esp_fill_random(chal.challenge, NONCE_LEN);

    xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
    session_t *sess = sess_find(mac);
    if (!sess) sess = sess_alloc(mac);
    if (sess) {
        memcpy(sess->challenge, chal.challenge, NONCE_LEN);
        sess->issued_at = unix_now();
    }
    xSemaphoreGive(s_sess_mutex);

    mesh_addr_t dest;
    memcpy(dest.addr, mac, 6);
    mesh_data_t tx = { .data  = (uint8_t *)&chal, .size  = sizeof(chal),
                       .proto = MESH_PROTO_BIN,    .tos   = MESH_TOS_P2P };
    esp_mesh_send(&dest, &tx, MESH_DATA_FROMDS, NULL, 0);
}

void auth_revoke_node(const uint8_t *mac)
{
    ESP_LOGW(TAG, "Revoking " MACSTR, MAC2STR(mac));

    xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
    session_t *s = sess_find(mac);
    if (s) memset(s, 0, sizeof(*s));
    xSemaphoreGive(s_sess_mutex);

    pkt_hdr_t rev = { .type = PKT_SESSION_REVOKE };
    memcpy(rev.src_id, s_my_mac, 6);
    mesh_addr_t dest;
    memcpy(dest.addr, mac, 6);
    mesh_data_t tx = { .data  = (uint8_t *)&rev, .size  = sizeof(rev),
                       .proto = MESH_PROTO_BIN,   .tos   = MESH_TOS_P2P };
    esp_mesh_send(&dest, &tx, MESH_DATA_FROMDS, NULL, 0);
}

bool auth_is_node_authenticated(const uint8_t *mac)
{
    if (!esp_mesh_is_root()) return s_authenticated;
    xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
    session_t *s = sess_find(mac);
    bool ok = s && s->authenticated;
    xSemaphoreGive(s_sess_mutex);
    return ok;
}


//kick node 
static void kick_node(const mesh_addr_t *node)
{
    pkt_hdr_t kick = { .type = PKT_KICK };
    mesh_data_t tx = {
        .data  = (uint8_t *)&kick,
        .size  = sizeof(kick),
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P
    };
    esp_mesh_send((mesh_addr_t *)node, &tx, MESH_DATA_FROMDS, NULL, 0);
    ESP_LOGW(TAG, "Kick sent to " MACSTR, MAC2STR(node->addr));
}
/* ── session watchdog (root only) for session expiry ─────────────────────────── */
static void watchdog_task(void *arg)
{
    ESP_LOGI(TAG, "Session watchdog started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); /* check every minute */
        if (!esp_mesh_is_root()) continue;

        uint32_t now = unix_now();
        xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
        for (int i = 0; i < SESSION_MAX; i++) {
            session_t *s = &s_sessions[i];
            if (!s->authenticated) continue;
            if (now >= s->session_expires) {
                uint8_t mac[6];
                memcpy(mac, s->node_id, 6);
                s->authenticated = false;
                xSemaphoreGive(s_sess_mutex);
                ESP_LOGI(TAG, "Session expired → re-challenge " MACSTR, MAC2STR(mac));
                auth_send_challenge(mac);
                xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
            }
        }
        xSemaphoreGive(s_sess_mutex);
    }
}

/* ── public: init ─────────────────────────────────────────── */
void auth_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    s_sess_mutex = xSemaphoreCreateMutex();

    esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);
    auth_load_credentials();

    xTaskCreatePinnedToCore(watchdog_task, "auth_wdog", 2560, NULL,
                            tskIDLE_PRIORITY + 1, &s_watchdog_handle, 1);
    ESP_LOGI(TAG, "Auth init — " MACSTR, MAC2STR(s_my_mac));
}



void auth_deinit(void)
{
    /* 1. Stop watchdog task */
    if (s_watchdog_handle) {
        vTaskDelete(s_watchdog_handle);
        s_watchdog_handle = NULL;
    }
 
    /* 2. Clear session table under mutex then destroy it */
    if (s_sess_mutex) {
        xSemaphoreTake(s_sess_mutex, portMAX_DELAY);
        memset(s_sessions, 0, sizeof(s_sessions));
        xSemaphoreGive(s_sess_mutex);
        vSemaphoreDelete(s_sess_mutex);
        s_sess_mutex = NULL;
    }
 
    /* 3. Clear key material from RAM */
    memset(s_my_key, 0, HMAC_KEY_LEN);
    memset(s_my_mac, 0, sizeof(s_my_mac));
    s_key_loaded      = false;
    s_authenticated   = false;
    s_session_expires = 0;
    s_auth_retries    = 0;
    s_auth_cb         = NULL;
 

 
    ESP_LOGI(TAG, "Auth deinit done");
}
 