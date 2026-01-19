#include "uwl_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "uwl_ble_gatt.h"
#include "uwl_wifi_softap.h"
#include "uwl_ws.h"

static const char *TAG = "uwl_http";

extern const unsigned char _binary_index_html_start[] asm("_binary_index_html_start");
extern const unsigned char _binary_index_html_end[] asm("_binary_index_html_end");

extern const unsigned char _binary_control_html_start[] asm("_binary_control_html_start");
extern const unsigned char _binary_control_html_end[] asm("_binary_control_html_end");

extern const unsigned char _binary_config_html_start[] asm("_binary_config_html_start");
extern const unsigned char _binary_config_html_end[] asm("_binary_config_html_end");

extern const unsigned char _binary_app_js_start[] asm("_binary_app_js_start");
extern const unsigned char _binary_app_js_end[] asm("_binary_app_js_end");

extern const unsigned char _binary_style_css_start[] asm("_binary_style_css_start");
extern const unsigned char _binary_style_css_end[] asm("_binary_style_css_end");

static esp_err_t uwl_http_send_asset(httpd_req_t *req,
                                    const unsigned char *start,
                                    const unsigned char *end,
                                    const char *content_type)
{
    if (content_type) {
        httpd_resp_set_type(req, content_type);
    }
    // Avoid aggressive browser caching (mobile browsers can cache embedded assets very hard).
    // This ensures UI updates show up immediately after flashing new firmware.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    const size_t len = (size_t)(end - start);
    return httpd_resp_send(req, (const char *)start, len);
}

static esp_err_t uwl_http_root_handler(httpd_req_t *req)
{
    // Keep / as the default entrypoint (control page).
    return uwl_http_send_asset(req, _binary_control_html_start, _binary_control_html_end, "text/html");
}

static esp_err_t uwl_http_control_handler(httpd_req_t *req)
{
    return uwl_http_send_asset(req, _binary_control_html_start, _binary_control_html_end, "text/html");
}

static esp_err_t uwl_http_config_handler(httpd_req_t *req)
{
    return uwl_http_send_asset(req, _binary_config_html_start, _binary_config_html_end, "text/html");
}

static esp_err_t uwl_http_app_js_handler(httpd_req_t *req)
{
    return uwl_http_send_asset(req, _binary_app_js_start, _binary_app_js_end, "application/javascript");
}

static esp_err_t uwl_http_style_css_handler(httpd_req_t *req)
{
    return uwl_http_send_asset(req, _binary_style_css_start, _binary_style_css_end, "text/css");
}

static esp_err_t uwl_http_favicon_handler(httpd_req_t *req)
{
    // No icon; returning 204 avoids noisy 404 in browsers
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t uwl_http_api_status_handler(httpd_req_t *req)
{
    const int sta = uwl_wifi_softap_get_sta_count();
    const size_t ws = uwl_ws_get_client_count();
    const bool ble_conn = uwl_ble_is_connected();
    const bool ble_notify = uwl_ble_is_state_notify_enabled();

    char buf[192];
    const int n = snprintf(buf, sizeof(buf),
                           "{\"sta_count\":%d,\"ws_clients\":%u,\"ble_connected\":%s,\"ble_notify\":%s}",
                           sta,
                           (unsigned)ws,
                           ble_conn ? "true" : "false",
                           ble_notify ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, (n < 0) ? HTTPD_RESP_USE_STRLEN : n);
}

esp_err_t uwl_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // With multiple clients, browsers open several parallel connections
    // (html/css/js + /api/status polling + WebSocket). If we hit the default
    // socket cap, LRU purge may drop the WS connection causing UI flicker.
    //
    // Increase sockets within LWIP limit and keep LRU purge as a safety net.
    // NOTE:
    // esp_http_server internally consumes a few sockets (typically 3),
    // so max_open_sockets must be <= (CONFIG_LWIP_MAX_SOCKETS - internal).
    // With CONFIG_LWIP_MAX_SOCKETS=10, the max allowed is 7.
    #if defined(CONFIG_LWIP_MAX_SOCKETS)
    config.max_open_sockets = CONFIG_LWIP_MAX_SOCKETS - 3;
    if (config.max_open_sockets < 4) config.max_open_sockets = 4;
    #else
    config.max_open_sockets = 7;
    #endif
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = uwl_http_root_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t control = {
        .uri = "/control",
        .method = HTTP_GET,
        .handler = uwl_http_control_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &control);

    httpd_uri_t config_page = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = uwl_http_config_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &config_page);

    httpd_uri_t appjs = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = uwl_http_app_js_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &appjs);

    httpd_uri_t css = {
        .uri = "/style.css",
        .method = HTTP_GET,
        .handler = uwl_http_style_css_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &css);

    // Avoid noisy 404 in browsers
    httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = uwl_http_favicon_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &favicon);

    httpd_uri_t api_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = uwl_http_api_status_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &api_status);

    ESP_ERROR_CHECK(uwl_ws_register(server));

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

