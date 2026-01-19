#include "uwl_usb_console.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

#include "uwl_ble_gatt.h"
#include "uwl_io_state.h"
#include "uwl_wifi_softap.h"
#include "uwl_ws.h"

static const char *TAG = "uwl_usb_console";
static esp_console_repl_t *s_repl = NULL;

static int uwl_cmd_gpio(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("  gpio list\n");
        printf("  gpio get <pin>\n");
        printf("  gpio set <pin> <0|1>\n");
        return 0;
    }

    if (strcmp(argv[1], "list") == 0) {
        size_t count = 0;
        const uwl_io_entry_t *entries = uwl_io_state_entries(&count);
        for (size_t i = 0; i < count; i++) {
            printf("GPIO%d dir=%s value=%u\n",
                   entries[i].pin,
                   entries[i].dir == UWL_IO_DIR_OUTPUT ? "out" : "in",
                   (unsigned)entries[i].value);
        }
        return 0;
    }

    if (strcmp(argv[1], "get") == 0) {
        if (argc < 3) {
            printf("Usage: gpio get <pin>\n");
            return 1;
        }
        const int pin = atoi(argv[2]);
        uint8_t v = 0;
        const esp_err_t err = uwl_io_state_get(pin, &v);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("GPIO%d=%u\n", pin, (unsigned)v);
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            printf("Usage: gpio set <pin> <0|1>\n");
            return 1;
        }
        const int pin = atoi(argv[2]);
        const int value = atoi(argv[3]);
        const esp_err_t err = uwl_io_state_set(pin, value ? 1 : 0, UWL_IO_SOURCE_USB);
        if (err != ESP_OK) {
            printf("ERR %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("OK\n");
        return 0;
    }

    printf("Unknown subcommand: %s\n", argv[1]);
    return 1;
}

static int uwl_cmd_wifi(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("wifi sta_count=%d\n", uwl_wifi_softap_get_sta_count());
    return 0;
}

static int uwl_cmd_ws(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("ws clients=%u\n", (unsigned)uwl_ws_get_client_count());
    return 0;
}

static int uwl_cmd_ble(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("ble connected=%u notify=%u\n",
           (unsigned)(uwl_ble_is_connected() ? 1 : 0),
           (unsigned)(uwl_ble_is_state_notify_enabled() ? 1 : 0));
    return 0;
}

static int uwl_cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("status:\n");
    printf("  wifi sta_count=%d\n", uwl_wifi_softap_get_sta_count());
    printf("  ws clients=%u\n", (unsigned)uwl_ws_get_client_count());
    printf("  ble connected=%u notify=%u\n",
           (unsigned)(uwl_ble_is_connected() ? 1 : 0),
           (unsigned)(uwl_ble_is_state_notify_enabled() ? 1 : 0));
    return 0;
}

esp_err_t uwl_usb_console_start(void)
{
    if (s_repl) return ESP_OK;

    // Register commands
    esp_console_cmd_t gpio_cmd = {
        .command = "gpio",
        .help = "GPIO control: list/get/set",
        .hint = NULL,
        .func = &uwl_cmd_gpio,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&gpio_cmd));

    esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "Wi-Fi status (SoftAP): sta_count",
        .hint = NULL,
        .func = &uwl_cmd_wifi,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));

    esp_console_cmd_t ws_cmd = {
        .command = "ws",
        .help = "WebSocket status: online clients count",
        .hint = NULL,
        .func = &uwl_cmd_ws,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ws_cmd));

    esp_console_cmd_t ble_cmd = {
        .command = "ble",
        .help = "BLE status: connected/notify",
        .hint = NULL,
        .func = &uwl_cmd_ble,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_cmd));

    esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "Print system status (wifi/ws/ble)",
        .hint = NULL,
        .func = &uwl_cmd_status,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "uwl> ";

    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &s_repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_console_new_repl_usb_serial_jtag failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_console_start_repl(s_repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_console_start_repl failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB console started (type: gpio list/get/set)");
    return ESP_OK;
}

