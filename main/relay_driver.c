#include "relay_driver.h"

#include "driver/gpio.h"
#include "hardware_config.h"

static const gpio_num_t s_relay_gpios[RELAY_COUNT] = {
    RELAY1_GPIO,
    RELAY2_GPIO,
    RELAY3_GPIO,
    RELAY4_GPIO,
};

esp_err_t relay_driver_init(void)
{
    for (size_t i = 0; i < RELAY_COUNT; ++i) {
        esp_err_t err = gpio_set_level(s_relay_gpios[i],
                                       !RELAY_ACTIVE_LEVEL);
        if (err != ESP_OK) {
            return err;
        }
    }

    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << RELAY1_GPIO) | (1ULL << RELAY2_GPIO) |
                        (1ULL << RELAY3_GPIO) | (1ULL << RELAY4_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

esp_err_t relay_driver_set(size_t relay_index, bool on)
{
    if (relay_index >= RELAY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    return gpio_set_level(s_relay_gpios[relay_index],
                          on ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
}
