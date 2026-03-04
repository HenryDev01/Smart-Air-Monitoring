#include <stdbool.h>

#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

void wifi_init_sta(void);
bool wifi_is_connected(void);

#endif