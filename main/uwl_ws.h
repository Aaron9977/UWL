#pragma once

#include "esp_err.h"
#include <stddef.h>

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uwl_ws_register(httpd_handle_t server);
size_t uwl_ws_get_client_count(void);

#ifdef __cplusplus
}
#endif

