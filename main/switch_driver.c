#include "switch_driver.h"

#include "driver/gpio.h"
#include "hardware_config.h"

static const gpio_num_t s_switch_gpios[RELAY_COUNT] = {
    SWITCH1_GPIO,
    SWITCH2_GPIO,
    SWITCH3_GPIO,
    SWITCH4_GPIO,
};

esp_err_t switch_driver_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << SWITCH1_GPIO) | (1ULL << SWITCH2_GPIO) |
                        (1ULL << SWITCH3_GPIO) | (1ULL << SWITCH4_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

esp_err_t switch_driver_get_state(size_t switch_index, bool *on)
{
    if (switch_index >= RELAY_COUNT || on == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int level = gpio_get_level(s_switch_gpios[switch_index]);
    *on = (level == SWITCH_ON_LEVEL);
    return ESP_OK;
}
