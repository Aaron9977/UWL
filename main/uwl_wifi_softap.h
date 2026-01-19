#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uwl_wifi_softap_start(void);
int uwl_wifi_softap_get_sta_count(void);

#ifdef __cplusplus
}
#endif

