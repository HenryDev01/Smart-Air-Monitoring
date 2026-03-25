#pragma once
#include "../../configuration/air_mesh.h"

/* Credential management */
esp_err_t auth_load_credentials(void);
esp_err_t auth_provision_key(const uint8_t *key, size_t key_len);

/* Module init */
void auth_init(void);
void auth_deinit(void);


/* Node-side (client) */
void auth_send_join_request(void);
void auth_handle_join_response(const pkt_join_resp_t *pkt);
void auth_handle_challenge(const pkt_challenge_t *pkt);

/* Root-side (server) */
void auth_handle_join_request(const pkt_join_req_t *req, const mesh_addr_t *from);
void auth_handle_challenge_response(const pkt_challenge_resp_t *pkt, const mesh_addr_t *from);
void auth_send_challenge(const uint8_t *node_mac);
void auth_revoke_node(const uint8_t *node_mac);

/* Query */
bool auth_is_node_authenticated(const uint8_t *mac);

// callback type
typedef void (*auth_node_authenticated_cb_t)(const uint8_t *mac);

// register the callback
void auth_set_authenticated_cb(auth_node_authenticated_cb_t cb);

// kick node

static void kick_node(const mesh_addr_t *node);
