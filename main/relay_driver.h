#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t relay_driver_init(void);
esp_err_t relay_driver_set(size_t relay_index, bool on);
