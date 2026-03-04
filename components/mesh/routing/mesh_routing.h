#pragma once

#include "../../configuration/air_mesh.h"

void  routing_init(void);
void  routing_handle_hello(const pkt_hello_t *pkt, const mesh_addr_t *from);
void  routing_record_tx(const uint8_t *mac, bool acked);
void  routing_update_parent(void);
void  routing_invalidate_parent(void);
void  routing_reset_etx(void);
float routing_get_etx_to_root(void);