/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "XINGZHI_CUBE_OLED";

#define XINGZHI_POWER_HOLD_GPIO GPIO_NUM_21

void board_init(void)
{
    const gpio_config_t power_hold_cfg = {
        .pin_bit_mask = BIT64(XINGZHI_POWER_HOLD_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&power_hold_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure power hold GPIO: %s", esp_err_to_name(err));
        return;
    }
    err = gpio_set_level(XINGZHI_POWER_HOLD_GPIO, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable power hold GPIO: %s", esp_err_to_name(err));
    }
}
