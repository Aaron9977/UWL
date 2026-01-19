#include "uwl_io_state.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "uwl_gpio.h"

static const char *TAG = "uwl_io_state";

#ifndef CONFIG_UWL_GPIO_OUT1
#define CONFIG_UWL_GPIO_OUT1 18
#endif
#ifndef CONFIG_UWL_GPIO_OUT2
#define CONFIG_UWL_GPIO_OUT2 19
#endif
#ifndef CONFIG_UWL_GPIO_OUT3
#define CONFIG_UWL_GPIO_OUT3 20
#endif
#ifndef CONFIG_UWL_GPIO_OUT4
#define CONFIG_UWL_GPIO_OUT4 21
#endif
#ifndef CONFIG_UWL_GPIO_IN1
#define CONFIG_UWL_GPIO_IN1 10
#endif

// Conservative: avoid USB D-/D+ default pins on ESP32-C6
static bool uwl_is_forbidden_pin(int pin)
{
    if ((pin == 12) || (pin == 13)) return true; // USB D-/D+

    // Avoid clobbering the onboard status LED (WS2812 data pin) if enabled.
    #if defined(CONFIG_UWL_ENABLE_STATUS_LED) && CONFIG_UWL_ENABLE_STATUS_LED
    if (pin == CONFIG_UWL_STATUS_LED_GPIO) return true;
    #endif

    return false;
}

static bool uwl_is_valid_pin(int pin)
{
    return pin >= 0 && pin <= 30 && !uwl_is_forbidden_pin(pin);
}

// Expose more pins so the Web UI can control header GPIOs without recompiling.
static uwl_io_entry_t s_entries[32];
static size_t s_entry_count = 0;

typedef struct {
    uwl_io_listener_fn fn;
    void *ctx;
} uwl_listener_t;

static uwl_listener_t s_listeners[8];
static size_t s_listener_count = 0;

static SemaphoreHandle_t s_lock = NULL;
static QueueHandle_t s_evt_q = NULL;

static void uwl_emit_event_from_task(const uwl_io_event_t *evt)
{
    if (!evt) return;

    // Copy listeners under lock; call callbacks without holding it
    uwl_listener_t listeners_local[8];
    size_t n = 0;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_listener_count;
    if (n > 8) n = 8;
    memcpy(listeners_local, s_listeners, n * sizeof(uwl_listener_t));
    if (s_lock) xSemaphoreGive(s_lock);

    for (size_t i = 0; i < n; i++) {
        if (listeners_local[i].fn) {
            listeners_local[i].fn(evt, listeners_local[i].ctx);
        }
    }
}

static int uwl_find_entry_idx(int pin)
{
    for (size_t i = 0; i < s_entry_count; i++) {
        if (s_entries[i].pin == pin) return (int)i;
    }
    return -1;
}

static void uwl_io_dispatcher_task(void *arg)
{
    (void)arg;
    uwl_io_event_t evt;
    while (true) {
        if (xQueueReceive(s_evt_q, &evt, portMAX_DELAY) == pdTRUE) {
            // Keep cached snapshot consistent in one place (task context)
            if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
            const int idx = uwl_find_entry_idx(evt.pin);
            if (idx >= 0) {
                s_entries[idx].value = evt.value ? 1 : 0;
            }
            if (s_lock) xSemaphoreGive(s_lock);
            uwl_emit_event_from_task(&evt);
        }
    }
}

static void uwl_add_entry(int pin, uwl_io_dir_t dir)
{
    if (!uwl_is_valid_pin(pin)) return;
    if (s_entry_count >= (sizeof(s_entries) / sizeof(s_entries[0]))) return;
    if (uwl_find_entry_idx(pin) >= 0) return;

    s_entries[s_entry_count++] = (uwl_io_entry_t){
        .pin = pin,
        .dir = dir,
        .value = 0,
    };
}

#if defined(CONFIG_UWL_ENABLE_HEADER_PRESET) && CONFIG_UWL_ENABLE_HEADER_PRESET
static void uwl_add_header_preset_entries(void)
{
    // ESP32-C6 DevKitC-1 header (J1/J3) - safe subset:
    // - Exclude strap pins: 0,1,4,5,9,15 (can affect boot)
    // - Exclude USB pins: 12,13 (handled by uwl_is_forbidden_pin)
    // - Exclude status LED pin: usually GPIO8 (handled by uwl_is_forbidden_pin)
    // - Exclude UART pins 16/17 by default (often used as console)
    static const int out_pins[] = {
        2, 3, 6, 7, 10, 11, 18, 19, 20, 21, 22, 23,
    };

    for (size_t i = 0; i < sizeof(out_pins) / sizeof(out_pins[0]); i++) {
        uwl_add_entry(out_pins[i], UWL_IO_DIR_OUTPUT);
    }
}
#endif

esp_err_t uwl_io_state_init(void)
{
    if (s_lock) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_evt_q = xQueueCreate(32, sizeof(uwl_io_event_t));
    if (!s_evt_q) return ESP_ERR_NO_MEM;

    // Build whitelist
    s_entry_count = 0;
    uwl_add_entry(CONFIG_UWL_GPIO_OUT1, UWL_IO_DIR_OUTPUT);
    uwl_add_entry(CONFIG_UWL_GPIO_OUT2, UWL_IO_DIR_OUTPUT);
    uwl_add_entry(CONFIG_UWL_GPIO_OUT3, UWL_IO_DIR_OUTPUT);
    uwl_add_entry(CONFIG_UWL_GPIO_OUT4, UWL_IO_DIR_OUTPUT);
    uwl_add_entry(CONFIG_UWL_GPIO_IN1, UWL_IO_DIR_INPUT);

    #if defined(CONFIG_UWL_ENABLE_HEADER_PRESET) && CONFIG_UWL_ENABLE_HEADER_PRESET
    uwl_add_header_preset_entries();
    #endif

    if (s_entry_count == 0) {
        ESP_LOGE(TAG, "No GPIO entries configured");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_ERROR_CHECK(uwl_gpio_init());

    // Configure GPIOs + read initial levels
    for (size_t i = 0; i < s_entry_count; i++) {
        const int pin = s_entries[i].pin;
        esp_err_t err = ESP_OK;
        if (s_entries[i].dir == UWL_IO_DIR_OUTPUT) {
            err = uwl_gpio_config_output(pin, 0);
        } else {
            err = uwl_gpio_config_input_with_isr(pin, true, false);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GPIO init failed pin=%d dir=%d: %s", pin, (int)s_entries[i].dir, esp_err_to_name(err));
            return err;
        }

        uint8_t level = 0;
        (void)uwl_gpio_get_level(pin, &level);
        s_entries[i].value = level ? 1 : 0;
    }

    xTaskCreate(uwl_io_dispatcher_task, "uwl_io_evt", 4096, NULL, 10, NULL);

    // Emit boot snapshot events (optional: one per pin)
    for (size_t i = 0; i < s_entry_count; i++) {
        const uwl_io_event_t evt = {
            .pin = s_entries[i].pin,
            .value = s_entries[i].value,
            .dir = s_entries[i].dir,
            .reason = UWL_IO_REASON_BOOT,
            .source = UWL_IO_SOURCE_LOCAL,
        };
        xQueueSend(s_evt_q, &evt, 0);
    }

    ESP_LOGI(TAG, "io_state init ok, entries=%u", (unsigned)s_entry_count);
    return ESP_OK;
}

const uwl_io_entry_t *uwl_io_state_entries(size_t *count_out)
{
    if (count_out) *count_out = s_entry_count;
    return s_entries;
}

esp_err_t uwl_io_state_add_listener(uwl_io_listener_fn fn, void *ctx)
{
    if (!fn) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_listener_count >= (sizeof(s_listeners) / sizeof(s_listeners[0]))) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_listeners[s_listener_count++] = (uwl_listener_t){ .fn = fn, .ctx = ctx };
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t uwl_io_state_get(int pin, uint8_t *value_out)
{
    if (!value_out) return ESP_ERR_INVALID_ARG;
    const int idx = uwl_find_entry_idx(pin);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    *value_out = s_entries[idx].value;
    return ESP_OK;
}

esp_err_t uwl_io_state_set(int pin, uint8_t value, uwl_io_source_t source)
{
    const int idx = uwl_find_entry_idx(pin);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    if (s_entries[idx].dir != UWL_IO_DIR_OUTPUT) return ESP_ERR_INVALID_STATE;

    const uint8_t v = value ? 1 : 0;
    const esp_err_t err = uwl_gpio_set_level(pin, v);
    if (err != ESP_OK) return err;

    // Update cached value
    s_entries[idx].value = v;

    const uwl_io_event_t evt = {
        .pin = pin,
        .value = v,
        .dir = s_entries[idx].dir,
        .reason = UWL_IO_REASON_SET_CMD,
        .source = source,
    };
    xQueueSend(s_evt_q, &evt, 0);
    return ESP_OK;
}

void uwl_io_state_on_input_edge_isr(int pin, uint8_t value)
{
    const int idx = uwl_find_entry_idx(pin);
    if (idx < 0) return;
    if (s_entries[idx].dir != UWL_IO_DIR_INPUT) return;

    const uint8_t v = value ? 1 : 0;
    // Avoid spamming identical events; read cached value without locking (best-effort)
    if (s_entries[idx].value == v) return;
    const uwl_io_event_t evt = {
        .pin = pin,
        .value = v,
        .dir = s_entries[idx].dir,
        .reason = UWL_IO_REASON_INPUT_EDGE,
        .source = UWL_IO_SOURCE_LOCAL,
    };

    BaseType_t hp_task_woken = pdFALSE;
    (void)xQueueSendFromISR(s_evt_q, &evt, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

