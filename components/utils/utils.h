#include <stdint.h>
#include <stdbool.h>

#define WIFI_MIN_RSSI_THRESHOLD -40

void sta_to_ap_mac(const uint8_t sta[6], uint8_t ap[6]);
void ap_to_sta_mac(const uint8_t ap[6], uint8_t sta[6]);
bool wifi_signal_strong_enough(void);
