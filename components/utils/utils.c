#include "utils.h"
#include <string.h>

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