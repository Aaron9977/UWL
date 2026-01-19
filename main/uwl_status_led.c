#include "uwl_status_led.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#else
// Keep clangd/linter parseable on host toolchains without ESP-IDF headers.
#include <stdbool.h>
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_ERROR_CHECK
#define ESP_ERROR_CHECK(x) (void)(x)
#endif
#ifndef portMAX_DELAY
#define portMAX_DELAY 0
#endif
typedef int TickType_t;
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(x) (x)
#endif
static inline void vTaskDelay(TickType_t x) { (void)x; }
typedef void *rmt_channel_handle_t;
typedef void *rmt_encoder_handle_t;
typedef struct { int loop_count; } rmt_transmit_config_t;
typedef struct { uint16_t duration0; uint16_t duration1; uint8_t level0; uint8_t level1; } rmt_symbol_word_t;
static inline esp_err_t rmt_transmit(rmt_channel_handle_t a, rmt_encoder_handle_t b, const void *c, size_t d, const void *e) { (void)a;(void)b;(void)c;(void)d;(void)e; return 0; }
static inline esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t a, int b) { (void)a;(void)b; return 0; }
#define ESP_LOGI(tag, fmt, ...) (void)0
#define ESP_LOGE(tag, fmt, ...) (void)0
#endif

#include "uwl_ble_gatt.h"
#include "uwl_wifi_softap.h"
#include "uwl_ws.h"

static const char *TAG = "uwl_led";

// ESP32-C6 DevKitC-1: board RGB is WS2812-compatible on GPIO8
#if !defined(CONFIG_UWL_ENABLE_STATUS_LED)
#define CONFIG_UWL_ENABLE_STATUS_LED 1
#endif
#ifndef CONFIG_UWL_STATUS_LED_GPIO
#define CONFIG_UWL_STATUS_LED_GPIO 8
#endif
#ifndef CONFIG_UWL_STATUS_LED_BRIGHTNESS
#define CONFIG_UWL_STATUS_LED_BRIGHTNESS 64
#endif

// RMT/WS2812 backend (no external component dependency)
static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static uint8_t s_pixels[3]; // GRB

// 10MHz resolution => 1 tick = 0.1us
#define UWL_RMT_RESOLUTION_HZ 10000000

static const rmt_symbol_word_t ws2812_zero = {
    .level0 = 1,
    .duration0 = 0.3 * UWL_RMT_RESOLUTION_HZ / 1000000, // T0H=0.3us
    .level1 = 0,
    .duration1 = 0.9 * UWL_RMT_RESOLUTION_HZ / 1000000, // T0L=0.9us
};

static const rmt_symbol_word_t ws2812_one = {
    .level0 = 1,
    .duration0 = 0.9 * UWL_RMT_RESOLUTION_HZ / 1000000, // T1H=0.9us
    .level1 = 0,
    .duration1 = 0.3 * UWL_RMT_RESOLUTION_HZ / 1000000, // T1L=0.3us
};

static const rmt_symbol_word_t ws2812_reset = {
    .level0 = 0,
    .duration0 = UWL_RMT_RESOLUTION_HZ / 1000000 * 50 / 2, // 25us
    .level1 = 0,
    .duration1 = UWL_RMT_RESOLUTION_HZ / 1000000 * 50 / 2, // 25us
};

static bool s_started = false;

static inline uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static size_t encoder_callback(const void *data, size_t data_size,
                               size_t symbols_written, size_t symbols_free,
                               rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;
    // Need at least 8 symbols to encode one byte
    if (symbols_free < 8) return 0;

    size_t data_pos = symbols_written / 8;
    const uint8_t *bytes = (const uint8_t *)data;
    if (data_pos < data_size) {
        size_t n = 0;
        const uint8_t v = bytes[data_pos];
        for (int bitmask = 0x80; bitmask != 0; bitmask >>= 1) {
            symbols[n++] = (v & bitmask) ? ws2812_one : ws2812_zero;
        }
        return n; // 8
    }

    // Reset frame and end transaction
    symbols[0] = ws2812_reset;
    *done = true;
    return 1;
}

static void tx_pixels(const uint8_t *bytes, size_t len)
{
    if (!s_chan || !s_encoder) return;
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    ESP_ERROR_CHECK(rmt_transmit(s_chan, s_encoder, bytes, len, &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_chan, portMAX_DELAY));
}

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    const int br = (int)CONFIG_UWL_STATUS_LED_BRIGHTNESS;
    r = clamp_u8((r * br) / 255);
    g = clamp_u8((g * br) / 255);
    b = clamp_u8((b * br) / 255);

    // WS2812 expects GRB order
    s_pixels[0] = g;
    s_pixels[1] = r;
    s_pixels[2] = b;
    tx_pixels(s_pixels, sizeof(s_pixels));
}

static void clear(void)
{
    memset(s_pixels, 0, sizeof(s_pixels));
    tx_pixels(s_pixels, sizeof(s_pixels));
}

static float breathe(float t)
{
    // 0..1
    const float pi = 3.14159265f;
    const float x = 0.5f - 0.5f * cosf(2.0f * pi * t);
    // make it less harsh at low end
    return x * x;
}

static void status_led_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(50);
    uint32_t tick = 0;

    // Boot animation: blue breathe for ~2s
    for (int i = 0; i < 40; i++) {
        const float k = breathe((float)i / 40.0f);
        set_rgb(0, 0, (uint8_t)(255 * k));
        vTaskDelay(period);
    }

    while (true) {
        const int sta = uwl_wifi_softap_get_sta_count();
        const size_t ws = uwl_ws_get_client_count();
        const bool ble_conn = uwl_ble_is_connected();

        // Priority:
        // 1) BLE connected: purple solid
        // 2) WS active: cyan breathing
        // 3) WiFi client connected: green solid (bright)
        // 4) idle SoftAP: green dim breathing slow
        if (ble_conn) {
            set_rgb(160, 0, 160);
        } else if (ws > 0) {
            const float k = breathe((float)(tick % 40) / 40.0f);
            set_rgb(0, (uint8_t)(180 * k), (uint8_t)(180 * k)); // cyan breathe
        } else if (sta > 0) {
            set_rgb(0, 255, 0);
        } else {
            const float k = breathe((float)(tick % 80) / 80.0f);
            set_rgb(0, (uint8_t)(120 * k), 0);
        }

        tick++;
        vTaskDelay(period);
    }
}

esp_err_t uwl_status_led_start(void)
{
    if (s_started) return ESP_OK;
    s_started = true;

    if (!CONFIG_UWL_ENABLE_STATUS_LED) {
        ESP_LOGI(TAG, "Status LED disabled");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Init status LED (WS2812) on GPIO%d via RMT", CONFIG_UWL_STATUS_LED_GPIO);

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = CONFIG_UWL_STATUS_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = UWL_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 2,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &s_chan));

    const rmt_simple_encoder_config_t enc_cfg = {
        .callback = encoder_callback,
    };
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&enc_cfg, &s_encoder));
    ESP_ERROR_CHECK(rmt_enable(s_chan));

    clear();

    xTaskCreate(status_led_task, "uwl_led", 3072, NULL, 6, NULL);
    return ESP_OK;
}

