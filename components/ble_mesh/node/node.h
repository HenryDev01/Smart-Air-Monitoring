#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t node_init(void);
esp_err_t node_deinit(void);
esp_err_t node_init_silent(void);
bool node_is_config_complete(void);
