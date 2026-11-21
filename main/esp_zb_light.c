/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier:  LicenseRef-Included
 *
 * Zigbee HA on/off relay example
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee_ota.h"
#include "nvs_flash.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl_utility.h"
#include "esp_zb_light.h"
#include "hardware_config.h"
#include "ota_update.h"
#include "relay_driver.h"
#include "switch_driver.h"

#if !defined CONFIG_ZB_ZCZR
#error Enable Zigbee Coordinator or Router in the project configuration.
#endif

static const char *TAG = "ZIGBEE_RELAY";
static TaskHandle_t s_switch_task_handle;
static esp_zb_zcl_ota_upgrade_client_variable_t s_ota_client_config = {
    .timer_query = HA_ESP_OTA_QUERY_INTERVAL_MINUTES,
    .hw_version = 1,
    .max_data_size = 64,
};

static uint8_t switch_endpoint(size_t switch_index)
{
    return HA_ESP_SWITCH_ENDPOINT_BASE + switch_index;
}

static uint8_t relay_endpoint(size_t relay_index)
{
    return HA_ESP_RELAY_ENDPOINT_BASE + relay_index;
}

static esp_err_t set_switch_attribute(size_t switch_index, bool on)
{
    uint8_t endpoint = switch_endpoint(switch_index);
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        endpoint,
        ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID,
        &on,
        false);
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "Failed to update switch sensor endpoint %u: 0x%x",
                 endpoint, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_attribute_report(uint8_t endpoint, uint16_t cluster_id,
                                       uint16_t attribute_id)
{
    esp_zb_zcl_report_attr_cmd_t report = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0,
            .dst_endpoint = 1,
            .src_endpoint = endpoint,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
        .attributeID = attribute_id,
    };
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&report);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to report cluster 0x%04x attribute 0x%04x on endpoint %u: %s",
                 cluster_id, attribute_id, endpoint, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t set_relay_attribute(size_t relay_index, bool on)
{
    uint8_t endpoint = relay_endpoint(relay_index);
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        endpoint,
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
        &on,
        false);
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "Failed to update On/Off attribute for endpoint %u: 0x%x",
                 endpoint, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_relay_report(size_t relay_index)
{
    return send_attribute_report(relay_endpoint(relay_index),
                                 ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                 ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
}

static esp_err_t send_switch_report(size_t switch_index)
{
    return send_attribute_report(switch_endpoint(switch_index),
                                 ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
                                 ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID);
}

static esp_err_t report_relay_state(size_t relay_index, bool on)
{
    esp_err_t err = set_relay_attribute(relay_index, on);
    if (err != ESP_OK) {
        return err;
    }
    return send_relay_report(relay_index);
}

static esp_err_t sync_switch_state(size_t relay_index, bool on)
{
    esp_err_t err = relay_driver_set(relay_index, on);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set relay %u: %s",
                 (unsigned)(relay_index + 1), esp_err_to_name(err));
        return err;
    }

    if (!esp_zb_lock_acquire(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Failed to acquire Zigbee lock while syncing relay %u",
                 (unsigned)(relay_index + 1));
        return ESP_FAIL;
    }
    err = report_relay_state(relay_index, on);
    if (err == ESP_OK) {
        err = set_switch_attribute(relay_index, on);
    }
    if (err == ESP_OK) {
        err = send_switch_report(relay_index);
    }
    esp_zb_lock_release();
    return err;
}

static void switch_monitor_task(void *arg)
{
    (void)arg;
    bool stable_state[RELAY_COUNT];
    bool candidate_state[RELAY_COUNT];
    TickType_t candidate_since[RELAY_COUNT];
    TickType_t debounce_ticks = pdMS_TO_TICKS(SWITCH_DEBOUNCE_MS);

    for (size_t i = 0; i < RELAY_COUNT; ++i) {
        if (switch_driver_get_state(i, &stable_state[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read switch %u at startup", (unsigned)(i + 1));
            stable_state[i] = false;
        }
        candidate_state[i] = stable_state[i];
        candidate_since[i] = xTaskGetTickCount();
    }

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        for (size_t i = 0; i < RELAY_COUNT; ++i) {
            bool sampled_state;
            if (switch_driver_get_state(i, &sampled_state) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read switch %u", (unsigned)(i + 1));
                continue;
            }

            if (sampled_state != candidate_state[i]) {
                candidate_state[i] = sampled_state;
                candidate_since[i] = now;
            } else if (candidate_state[i] != stable_state[i] &&
                       (TickType_t)(now - candidate_since[i]) >= debounce_ticks) {
                stable_state[i] = candidate_state[i];
                (void)sync_switch_state(i, stable_state[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t deferred_driver_init(void)
{
    ESP_RETURN_ON_ERROR(relay_driver_init(), TAG, "Relay GPIO initialization failed");
    ESP_RETURN_ON_ERROR(switch_driver_init(), TAG, "Switch GPIO initialization failed");

    for (size_t i = 0; i < RELAY_COUNT; ++i) {
        bool switch_on;
        ESP_RETURN_ON_ERROR(switch_driver_get_state(i, &switch_on), TAG,
                            "Failed to read switch during initialization");
        ESP_RETURN_ON_ERROR(relay_driver_set(i, switch_on), TAG,
                            "Failed to set relay from initial switch state");
        if (set_relay_attribute(i, switch_on) != ESP_OK) {
            return ESP_FAIL;
        }
        ESP_RETURN_ON_ERROR(set_switch_attribute(i, switch_on), TAG,
                            "Failed to set initial switch sensor state");
    }

    if (s_switch_task_handle == NULL &&
        xTaskCreate(switch_monitor_task, "switch_monitor", 3072, NULL, 5,
                    &s_switch_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create switch monitor task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(
        esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK,
        , TAG, "Failed to start Zigbee commissioning");
}

static void report_all_relay_states(void)
{
    for (size_t i = 0; i < RELAY_COUNT; ++i) {
        esp_err_t err = send_relay_report(i);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to report initial endpoint %u state: %s",
                     relay_endpoint(i), esp_err_to_name(err));
        }
        err = send_switch_report(i);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to report initial switch endpoint %u state: %s",
                     switch_endpoint(i), esp_err_to_name(err));
        }
    }
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *signal = signal_struct->p_app_signal;
    esp_err_t signal_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t signal_type = *signal;

    switch (signal_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (signal_status == ESP_OK) {
            esp_err_t err = deferred_driver_init();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Deferred driver initialization failed: %s",
                         esp_err_to_name(err));
                break;
            }
            err = ota_update_mark_running_valid();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to validate running firmware: %s",
                         esp_err_to_name(err));
                break;
            }
            err = esp_zb_ota_upgrade_client_query_interval_set(
                HA_ESP_RELAY_ENDPOINT_BASE, HA_ESP_OTA_QUERY_INTERVAL_MINUTES);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to configure Zigbee OTA query interval: %s",
                         esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "Relay and switch drivers initialized");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted");
            }
        } else {
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack: %s",
                     esp_err_to_name(signal_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (signal_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network (PAN ID: 0x%04hx, channel: %d, address: 0x%04hx)",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                     esp_zb_get_short_address());
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, extended_pan_id, sizeof(extended_pan_id),
                                     ESP_LOG_INFO);
            report_all_relay_states();
        } else {
            ESP_LOGI(TAG, "Network steering failed: %s",
                     esp_err_to_name(signal_status));
            esp_zb_scheduler_alarm(
                (esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(signal_type), signal_type,
                 esp_err_to_name(signal_status));
        break;
    }
}

static esp_err_t zb_attribute_handler(
    const esp_zb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message != NULL, ESP_FAIL, TAG, "Empty Zigbee message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS,
                        ESP_ERR_INVALID_ARG, TAG,
                        "Zigbee attribute update failed with status %d",
                        message->info.status);

    if (message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF ||
        message->attribute.id != ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID ||
        message->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
        return ESP_OK;
    }

    if (message->info.dst_endpoint < HA_ESP_RELAY_ENDPOINT_BASE ||
        message->info.dst_endpoint >= HA_ESP_RELAY_ENDPOINT_BASE + RELAY_COUNT) {
        return ESP_OK;
    }
    size_t relay_index = message->info.dst_endpoint - HA_ESP_RELAY_ENDPOINT_BASE;
    ESP_RETURN_ON_FALSE(message->attribute.data.value != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "Missing On/Off attribute value");

    bool on = *(bool *)message->attribute.data.value;
    esp_err_t err = relay_driver_set(relay_index, on);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Relay %u set to %s", (unsigned)(relay_index + 1),
                 on ? "ON" : "OFF");
    }
    return err;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message)
{
    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        return zb_attribute_handler(
            (const esp_zb_zcl_set_attr_value_message_t *)message);
    }
    if (callback_id == ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID) {
        return ota_update_handle(
            (const esp_zb_zcl_ota_upgrade_value_message_t *)message);
    }
    ESP_LOGW(TAG, "Unhandled Zigbee action 0x%x", callback_id);
    return ESP_OK;
}

static void esp_zb_task(void *pvParameters)
{
    (void)pvParameters;
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_ep_list_t *endpoint_list = esp_zb_ep_list_create();
    ESP_ERROR_CHECK(endpoint_list == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    zcl_basic_manufacturer_info_t manufacturer_info = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    for (size_t i = 0; i < RELAY_COUNT; ++i) {
        esp_zb_on_off_light_cfg_t light_config =
            ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
        esp_zb_cluster_list_t *cluster_list =
            esp_zb_on_off_light_clusters_create(&light_config);
        ESP_ERROR_CHECK(cluster_list == NULL ? ESP_ERR_NO_MEM : ESP_OK);
        if (i == 0) {
            esp_zb_ota_cluster_cfg_t ota_config = {
                .ota_upgrade_file_version = HA_ESP_OTA_FILE_VERSION,
                .ota_upgrade_manufacturer = HA_ESP_OTA_MANUFACTURER_CODE,
                .ota_upgrade_image_type = HA_ESP_OTA_IMAGE_TYPE,
                .ota_min_block_reque = 0,
                .ota_upgrade_file_offset = ESP_ZB_ZCL_OTA_UPGRADE_FILE_OFFSET_DEF_VALUE,
                .ota_upgrade_downloaded_file_ver =
                    ESP_ZB_ZCL_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_DEF_VALUE,
                .ota_upgrade_server_id = ESP_ZB_ZCL_OTA_UPGRADE_SERVER_DEF_VALUE,
                .ota_image_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_IMAGE_STATUS_DEF_VALUE,
            };
            esp_zb_attribute_list_t *ota_cluster =
                esp_zb_ota_cluster_create(&ota_config);
            ESP_ERROR_CHECK(ota_cluster == NULL ? ESP_ERR_NO_MEM : ESP_OK);
            ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(
                ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID,
                &s_ota_client_config));
            ESP_ERROR_CHECK(esp_zb_cluster_list_add_ota_cluster(
                cluster_list, ota_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));
        }

        esp_zb_endpoint_config_t endpoint_config = {
            .endpoint = relay_endpoint(i),
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
            .app_device_version = 1,
        };
        ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(endpoint_list, cluster_list,
                                              endpoint_config));
        esp_zcl_utility_add_ep_basic_manufacturer_info(
            endpoint_list, endpoint_config.endpoint, &manufacturer_info);
    }

    static uint8_t switch_descriptions[RELAY_COUNT][9] = {
        {8, 'S', 'w', 'i', 't', 'c', 'h', ' ', '1'},
        {8, 'S', 'w', 'i', 't', 'c', 'h', ' ', '2'},
        {8, 'S', 'w', 'i', 't', 'c', 'h', ' ', '3'},
        {8, 'S', 'w', 'i', 't', 'c', 'h', ' ', '4'},
    };
    for (size_t i = 0; i < RELAY_COUNT; ++i) {
        esp_zb_basic_cluster_cfg_t basic_config = {
            .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
            .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
        };
        esp_zb_binary_input_cluster_cfg_t input_config = {
            .out_of_service = false,
            .status_flags = ESP_ZB_ZCL_BINARY_INPUT_STATUS_FLAG_NORMAL,
            .present_value = false,
        };
        esp_zb_attribute_list_t *basic_cluster =
            esp_zb_basic_cluster_create(&basic_config);
        esp_zb_attribute_list_t *input_cluster =
            esp_zb_binary_input_cluster_create(&input_config);
        ESP_ERROR_CHECK(basic_cluster == NULL || input_cluster == NULL
                            ? ESP_ERR_NO_MEM : ESP_OK);
        ESP_ERROR_CHECK(esp_zb_binary_input_cluster_add_attr(
            input_cluster, ESP_ZB_ZCL_ATTR_BINARY_INPUT_DESCRIPTION_ID,
            switch_descriptions[i]));

        esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
        ESP_ERROR_CHECK(cluster_list == NULL ? ESP_ERR_NO_MEM : ESP_OK);
        ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(
            cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
        ESP_ERROR_CHECK(esp_zb_cluster_list_add_binary_input_cluster(
            cluster_list, input_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

        esp_zb_endpoint_config_t endpoint_config = {
            .endpoint = switch_endpoint(i),
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id = HA_ESP_ON_OFF_SENSOR_DEVICE_ID,
            .app_device_version = 1,
        };
        ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(endpoint_list, cluster_list,
                                              endpoint_config));
        esp_zcl_utility_add_ep_basic_manufacturer_info(
            endpoint_list, endpoint_config.endpoint, &manufacturer_info);
    }

    ESP_ERROR_CHECK(esp_zb_device_register(endpoint_list));
    esp_zb_core_action_handler_register(zb_action_handler);
    ESP_ERROR_CHECK(
        esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    if (xTaskCreate(esp_zb_task, "Zigbee_main", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
    }
}
