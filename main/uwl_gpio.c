#include "uwl_gpio.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"

#include "uwl_io_state.h"

static const char *TAG = "uwl_gpio";

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef ESP_INTR_FLAG_IRAM
#define ESP_INTR_FLAG_IRAM 0
#endif
#endif

static bool s_isr_service_installed = false;

static void IRAM_ATTR uwl_gpio_isr_handler(void *arg)
{
    const int pin = (int)(intptr_t)arg;
    const uint8_t level = (uint8_t)gpio_get_level(pin);
    uwl_io_state_on_input_edge_isr(pin, level);
}

esp_err_t uwl_gpio_init(void)
{
    if (!s_isr_service_installed) {
        const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
            return err;
        }
        s_isr_service_installed = true;
    }
    return ESP_OK;
}

esp_err_t uwl_gpio_config_output(int pin, uint8_t initial_value)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config output pin=%d failed: %s", pin, esp_err_to_name(err));
        return err;
    }
    err = gpio_set_level(pin, initial_value ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level pin=%d failed: %s", pin, esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t uwl_gpio_set_level(int pin, uint8_t value)
{
    const esp_err_t err = gpio_set_level(pin, value ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level pin=%d failed: %s", pin, esp_err_to_name(err));
    }
    return err;
}

esp_err_t uwl_gpio_get_level(int pin, uint8_t *value_out)
{
    if (!value_out) return ESP_ERR_INVALID_ARG;
    *value_out = (uint8_t)gpio_get_level(pin);
    return ESP_OK;
}

esp_err_t uwl_gpio_config_input_with_isr(int pin, bool pullup, bool pulldown)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pulldown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config input pin=%d failed: %s", pin, esp_err_to_name(err));
        return err;
    }

    err = uwl_gpio_init();
    if (err != ESP_OK) return err;

    err = gpio_isr_handler_add(pin, uwl_gpio_isr_handler, (void *)(intptr_t)pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add pin=%d failed: %s", pin, esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

