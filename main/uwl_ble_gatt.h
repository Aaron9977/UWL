#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uwl_ble_gatt_start(void);
bool uwl_ble_is_connected(void);
bool uwl_ble_is_state_notify_enabled(void);

#ifdef __cplusplus
}
#endif

