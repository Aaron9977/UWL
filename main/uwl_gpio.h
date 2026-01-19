#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uwl_gpio_init(void);

esp_err_t uwl_gpio_config_output(int pin, uint8_t initial_value);
esp_err_t uwl_gpio_set_level(int pin, uint8_t value);
esp_err_t uwl_gpio_get_level(int pin, uint8_t *value_out);

esp_err_t uwl_gpio_config_input_with_isr(int pin, bool pullup, bool pulldown);

#ifdef __cplusplus
}
#endif

