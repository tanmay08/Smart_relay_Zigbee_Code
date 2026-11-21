#pragma once

#include "esp_err.h"
#include "zcl/esp_zigbee_zcl_command.h"

esp_err_t ota_update_handle(
    const esp_zb_zcl_ota_upgrade_value_message_t *message);
esp_err_t ota_update_mark_running_valid(void);
