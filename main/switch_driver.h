#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t switch_driver_init(void);
esp_err_t switch_driver_get_state(size_t switch_index, bool *on);
