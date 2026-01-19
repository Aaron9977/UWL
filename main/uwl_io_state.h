#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UWL_IO_DIR_INPUT = 0,
    UWL_IO_DIR_OUTPUT = 1,
} uwl_io_dir_t;

typedef enum {
    UWL_IO_REASON_BOOT = 0,
    UWL_IO_REASON_INPUT_EDGE = 1,
    UWL_IO_REASON_SET_CMD = 2,
} uwl_io_reason_t;

typedef enum {
    UWL_IO_SOURCE_UNKNOWN = 0,
    UWL_IO_SOURCE_WIFI = 1,
    UWL_IO_SOURCE_USB = 2,
    UWL_IO_SOURCE_BLE = 3,
    UWL_IO_SOURCE_LOCAL = 4,
} uwl_io_source_t;

typedef struct {
    int pin;
    uwl_io_dir_t dir;
    uint8_t value; // 0/1
} uwl_io_entry_t;

typedef struct {
    int pin;
    uint8_t value; // 0/1
    uwl_io_dir_t dir;
    uwl_io_reason_t reason;
    uwl_io_source_t source;
} uwl_io_event_t;

typedef void (*uwl_io_listener_fn)(const uwl_io_event_t *evt, void *ctx);

esp_err_t uwl_io_state_init(void);

// Snapshot API (read-only, pointer valid for lifetime of app)
const uwl_io_entry_t *uwl_io_state_entries(size_t *count_out);

// Control/read API
esp_err_t uwl_io_state_get(int pin, uint8_t *value_out);
esp_err_t uwl_io_state_set(int pin, uint8_t value, uwl_io_source_t source);

// Subscribe to state change events (called from an internal dispatcher task)
esp_err_t uwl_io_state_add_listener(uwl_io_listener_fn fn, void *ctx);

// Used by GPIO ISR glue to inform input changes
void uwl_io_state_on_input_edge_isr(int pin, uint8_t value);

#ifdef __cplusplus
}
#endif

