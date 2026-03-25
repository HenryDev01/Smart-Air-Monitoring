#pragma once
#include "../../configuration/air_mesh.h"

void gossip_init(void);
void gossip_deinit(void);
void gossip_handle_packet(const pkt_gossip_t *pkt);
void gossip_send(const uint8_t *payload, uint8_t len, uint8_t ttl);

/**
 * @brief Override this weak function in main.c to handle gossip payloads.
 */
void gossip_process_payload(const uint8_t *payload, uint8_t len);
