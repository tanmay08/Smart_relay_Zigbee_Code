#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zb_light.h"
#include "ota_update.h"

static const char *TAG = "ZIGBEE_OTA";

#define OTA_FILE_IDENTIFIER 0x0BEEF11E
#define OTA_HEADER_VERSION 0x0100
#define OTA_HEADER_MIN_SIZE 56
#define OTA_HEADER_MAX_SIZE 256
#define OTA_HEADER_IMAGE_SIZE_OFFSET 52
#define OTA_IMAGE_TAG_ID 0x0000
#define OTA_SUBELEMENT_HEADER_SIZE 6

typedef enum {
    OTA_STREAM_DETECT_FORMAT,
    OTA_STREAM_RAW_FIRMWARE,
    OTA_STREAM_HEADER,
    OTA_STREAM_SUBELEMENT_HEADER,
    OTA_STREAM_FIRMWARE,
    OTA_STREAM_COMPLETE,
    OTA_STREAM_INVALID,
} ota_stream_state_t;

static const esp_partition_t *s_update_partition;
static esp_ota_handle_t s_ota_handle;
static bool s_update_active;
static ota_stream_state_t s_stream_state;
static uint8_t s_header[OTA_HEADER_MAX_SIZE];
static size_t s_header_bytes;
static size_t s_header_length;
static uint8_t s_subelement_header[OTA_SUBELEMENT_HEADER_SIZE];
static size_t s_subelement_header_bytes;
static size_t s_firmware_bytes_remaining;
static size_t s_firmware_bytes_written;
static size_t s_ota_bytes_received;
static size_t s_advertised_ota_size;
static size_t s_declared_ota_size;
static uint32_t s_expected_file_version;

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void ota_reset_stream(void)
{
    s_stream_state = OTA_STREAM_DETECT_FORMAT;
    s_header_bytes = 0;
    s_header_length = 0;
    s_subelement_header_bytes = 0;
    s_firmware_bytes_remaining = 0;
    s_firmware_bytes_written = 0;
    s_ota_bytes_received = 0;
    s_advertised_ota_size = 0;
    s_declared_ota_size = 0;
    s_expected_file_version = 0;
}

static void ota_reset_update(void)
{
    s_update_partition = NULL;
    s_update_active = false;
    ota_reset_stream();
}

static esp_err_t ota_write_firmware(const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0 || s_update_partition == NULL ||
        s_firmware_bytes_written > s_update_partition->size ||
        size > s_update_partition->size - s_firmware_bytes_written) {
        ESP_LOGE(TAG, "Firmware block exceeds OTA partition capacity");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_write(s_ota_handle, data, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write firmware block: %s",
                 esp_err_to_name(err));
        return err;
    }
    s_firmware_bytes_written += size;
    return ESP_OK;
}

static esp_err_t ota_parse_header(void)
{
    if (read_le32(s_header) != OTA_FILE_IDENTIFIER ||
        read_le16(&s_header[4]) != OTA_HEADER_VERSION) {
        ESP_LOGE(TAG, "Invalid or unsupported Zigbee OTA image header");
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_header_length = read_le16(&s_header[6]);
    if (s_header_length < OTA_HEADER_MIN_SIZE ||
        s_header_length > sizeof(s_header)) {
        ESP_LOGE(TAG, "Unsupported Zigbee OTA header length: %u",
                 (unsigned)s_header_length);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t ota_validate_header(void)
{
    if (read_le16(&s_header[10]) != HA_ESP_OTA_MANUFACTURER_CODE ||
        read_le16(&s_header[12]) != HA_ESP_OTA_IMAGE_TYPE ||
        read_le32(&s_header[14]) != s_expected_file_version ||
        s_expected_file_version <= HA_ESP_OTA_FILE_VERSION) {
        ESP_LOGE(TAG, "Zigbee OTA image metadata does not match this device");
        return ESP_ERR_INVALID_VERSION;
    }

    s_declared_ota_size = read_le32(&s_header[OTA_HEADER_IMAGE_SIZE_OFFSET]);
    if (s_declared_ota_size != s_advertised_ota_size ||
        s_declared_ota_size < s_header_length + OTA_SUBELEMENT_HEADER_SIZE ||
        s_declared_ota_size > s_update_partition->size +
            OTA_HEADER_MAX_SIZE + OTA_SUBELEMENT_HEADER_SIZE) {
        ESP_LOGE(TAG, "Invalid Zigbee OTA image length: %u",
                 (unsigned)s_declared_ota_size);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t ota_process_payload(const uint8_t *payload, size_t size)
{
    if (payload == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ota_bytes_received > s_advertised_ota_size ||
        size > s_advertised_ota_size - s_ota_bytes_received) {
        ESP_LOGE(TAG, "Received more data than declared in the OTA image");
        return ESP_ERR_INVALID_SIZE;
    }

    size_t offset = 0;
    while (offset < size) {
        if (s_stream_state == OTA_STREAM_DETECT_FORMAT) {
            if (payload[offset] == 0xE9) {
                s_stream_state = OTA_STREAM_RAW_FIRMWARE;
                esp_err_t err = ota_write_firmware(payload + offset, size - offset);
                if (err != ESP_OK) {
                    return err;
                }
                s_ota_bytes_received += size - offset;
                return ESP_OK;
            }
            if (payload[offset] != 0x1E) {
                ESP_LOGE(TAG, "Unrecognized Zigbee OTA payload format");
                return ESP_ERR_NOT_SUPPORTED;
            }
            s_stream_state = OTA_STREAM_HEADER;
        }

        if (s_stream_state == OTA_STREAM_RAW_FIRMWARE) {
            esp_err_t err = ota_write_firmware(payload + offset, size - offset);
            if (err != ESP_OK) {
                return err;
            }
            s_ota_bytes_received += size - offset;
            return ESP_OK;
        }

        if (s_stream_state == OTA_STREAM_HEADER) {
            s_header[s_header_bytes++] = payload[offset++];
            ++s_ota_bytes_received;

            if (s_header_bytes == 8) {
                esp_err_t err = ota_parse_header();
                if (err != ESP_OK) {
                    return err;
                }
            }
            if (s_header_length != 0 &&
                s_header_bytes == s_header_length) {
                esp_err_t err = ota_validate_header();
                if (err != ESP_OK) {
                    return err;
                }
                s_stream_state = OTA_STREAM_SUBELEMENT_HEADER;
            }
            continue;
        }

        if (s_stream_state == OTA_STREAM_SUBELEMENT_HEADER) {
            s_subelement_header[s_subelement_header_bytes++] = payload[offset++];
            ++s_ota_bytes_received;
            if (s_subelement_header_bytes == sizeof(s_subelement_header)) {
                if (read_le16(s_subelement_header) != OTA_IMAGE_TAG_ID) {
                    ESP_LOGE(TAG, "Unsupported OTA image subelement tag");
                    return ESP_ERR_NOT_SUPPORTED;
                }
                s_firmware_bytes_remaining =
                    read_le32(&s_subelement_header[2]);
                if (s_firmware_bytes_remaining == 0 ||
                    s_firmware_bytes_remaining > s_update_partition->size ||
                    s_declared_ota_size != s_header_length +
                        OTA_SUBELEMENT_HEADER_SIZE +
                        s_firmware_bytes_remaining) {
                    ESP_LOGE(TAG, "Invalid firmware subelement length: %u",
                             (unsigned)s_firmware_bytes_remaining);
                    return ESP_ERR_INVALID_SIZE;
                }
                s_stream_state = OTA_STREAM_FIRMWARE;
            }
            continue;
        }

        if (s_stream_state == OTA_STREAM_FIRMWARE) {
            size_t write_size = size - offset;
            if (write_size > s_firmware_bytes_remaining) {
                write_size = s_firmware_bytes_remaining;
            }
            esp_err_t err = ota_write_firmware(payload + offset, write_size);
            if (err != ESP_OK) {
                return err;
            }
            offset += write_size;
            s_ota_bytes_received += write_size;
            s_firmware_bytes_remaining -= write_size;
            if (s_firmware_bytes_remaining == 0) {
                s_stream_state = OTA_STREAM_COMPLETE;
            }
            continue;
        }

        if (s_stream_state == OTA_STREAM_COMPLETE ||
            s_stream_state == OTA_STREAM_INVALID) {
            ESP_LOGE(TAG, "Unexpected data after OTA image");
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

static void ota_restart_callback(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Restarting into updated firmware");
    esp_restart();
}

static esp_err_t ota_schedule_restart(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = ota_restart_callback,
        .name = "zigbee_ota_restart",
    };
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&timer_args, &timer);
    if (err == ESP_OK) {
        err = esp_timer_start_once(timer, 3000000);
        if (err != ESP_OK) {
            esp_err_t delete_err = esp_timer_delete(timer);
            if (delete_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to delete OTA restart timer: %s",
                         esp_err_to_name(delete_err));
            }
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to schedule OTA restart: %s",
                 esp_err_to_name(err));
        esp_restart();
    }
    return err;
}

static esp_err_t ota_start(
    const esp_zb_zcl_ota_upgrade_value_message_t *message)
{
    const esp_zb_ota_file_header_t *header = &message->ota_header;
    if (header->manufacturer_code != HA_ESP_OTA_MANUFACTURER_CODE ||
        header->image_type != HA_ESP_OTA_IMAGE_TYPE ||
        header->file_version <= HA_ESP_OTA_FILE_VERSION) {
        ESP_LOGW(TAG, "Ignoring incompatible or non-newer OTA image");
        return ESP_ERR_INVALID_VERSION;
    }
    if (s_update_active) {
        ESP_LOGW(TAG, "An OTA update is already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (s_update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA update partition is available");
        return ESP_ERR_NOT_FOUND;
    }
    if (header->image_size < OTA_HEADER_MIN_SIZE +
            OTA_SUBELEMENT_HEADER_SIZE ||
        header->image_size > s_update_partition->size +
            OTA_HEADER_MAX_SIZE + OTA_SUBELEMENT_HEADER_SIZE) {
        ESP_LOGE(TAG, "OTA image size is outside the supported range: %lu",
                 (unsigned long)header->image_size);
        s_update_partition = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    ota_reset_stream();
    s_advertised_ota_size = header->image_size;
    s_expected_file_version = header->file_version;
    esp_err_t err = esp_ota_begin(s_update_partition, OTA_SIZE_UNKNOWN,
                                  &s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin OTA update: %s", esp_err_to_name(err));
        ota_reset_update();
        return err;
    }

    s_update_active = true;
    ESP_LOGI(TAG, "Receiving firmware version %lu (%lu OTA bytes) into %s",
             (unsigned long)header->file_version,
             (unsigned long)header->image_size, s_update_partition->label);
    return ESP_OK;
}

static esp_err_t ota_finish(void)
{
    if (!s_update_active) {
        return ESP_ERR_INVALID_STATE;
    }

    bool raw_firmware_complete =
        s_stream_state == OTA_STREAM_RAW_FIRMWARE &&
        s_firmware_bytes_written > 0;
    bool container_complete =
        s_stream_state == OTA_STREAM_COMPLETE &&
        s_ota_bytes_received == s_declared_ota_size;
    if (!raw_firmware_complete && !container_complete) {
        ESP_LOGE(TAG, "OTA image is incomplete or malformed");
        esp_err_t abort_err = esp_ota_abort(s_ota_handle);
        if (abort_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to abort incomplete OTA image: %s",
                     esp_err_to_name(abort_err));
        }
        ota_reset_update();
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_ota_handle_t handle = s_ota_handle;
    const esp_partition_t *partition = s_update_partition;
    s_update_active = false;
    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP application image validation failed: %s",
                 esp_err_to_name(err));
        ota_reset_update();
        return err;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to select updated app partition: %s",
                 esp_err_to_name(err));
        ota_reset_update();
        return err;
    }

    ESP_LOGI(TAG, "OTA image validated (%u firmware bytes)",
             (unsigned)s_firmware_bytes_written);
    ota_reset_update();
    return ota_schedule_restart();
}

esp_err_t ota_update_handle(
    const esp_zb_zcl_ota_upgrade_value_message_t *message)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (message->upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        return ota_start(message);
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
        if (!s_update_active) {
            return ESP_ERR_INVALID_STATE;
        }
        {
            esp_err_t err =
                ota_process_payload(message->payload, message->payload_size);
            if (err != ESP_OK) {
                esp_err_t abort_err = esp_ota_abort(s_ota_handle);
                if (abort_err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to abort rejected OTA image: %s",
                             esp_err_to_name(abort_err));
                }
                ota_reset_update();
            }
            return err;
        }
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        return ota_finish();
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
        if (s_update_active) {
            esp_err_t err = esp_ota_abort(s_ota_handle);
            ota_reset_update();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to abort OTA update: %s",
                         esp_err_to_name(err));
                return err;
            }
        }
        ESP_LOGW(TAG, "Zigbee OTA server aborted the update");
        return ESP_OK;
    default:
        return ESP_OK;
    }
}

esp_err_t ota_update_mark_running_valid(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read running OTA state: %s",
                 esp_err_to_name(err));
        return err;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to confirm running firmware: %s",
                     esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "New firmware validated; rollback cancelled");
    }
    return ESP_OK;
}
