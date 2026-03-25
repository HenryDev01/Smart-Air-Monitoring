#pragma once
#include "esp_err.h"


esp_err_t mesh_init(void);
void mesh_deinit(void);
bool mesh_is_healthy(void);

