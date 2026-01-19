#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "uwl_http.h"
#include "uwl_io_state.h"
#include "uwl_wifi_softap.h"
#include "uwl_usb_console.h"
#include "uwl_ble_gatt.h"
#include "uwl_status_led.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "UWL boot");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(uwl_io_state_init());

    ESP_ERROR_CHECK(uwl_wifi_softap_start());
    ESP_ERROR_CHECK(uwl_http_start());

    // Board status LED (ESP32-C6 DevKitC-1: WS2812 on GPIO8)
    (void)uwl_status_led_start();

    // Optional interactive control from USB Serial/JTAG console
    #if defined(CONFIG_UWL_ENABLE_USB_CONSOLE) && CONFIG_UWL_ENABLE_USB_CONSOLE
    (void)uwl_usb_console_start();
    #endif

    // Optional BLE control channel (NimBLE)
    #if defined(CONFIG_UWL_ENABLE_BLE) && CONFIG_UWL_ENABLE_BLE
    (void)uwl_ble_gatt_start();
    #endif
}
