#include "uwl_ws.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "uwl_ble_gatt.h"
#include "uwl_io_state.h"
#include "uwl_wifi_softap.h"

static const char *TAG = "uwl_ws";

static httpd_handle_t s_server = NULL;
static SemaphoreHandle_t s_clients_lock = NULL;
static int s_clients[8];
static size_t s_client_count = 0;
static bool s_status_task_started = false;

size_t uwl_ws_get_client_count(void)
{
    if (!s_clients_lock) return 0;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    const size_t n = s_client_count;
    xSemaphoreGive(s_clients_lock);
    return n;
}

static void uwl_ws_client_add(int fd)
{
    if (!s_clients_lock) return;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_client_count; i++) {
        if (s_clients[i] == fd) {
            xSemaphoreGive(s_clients_lock);
            return;
        }
    }
    if (s_client_count < (sizeof(s_clients) / sizeof(s_clients[0]))) {
        s_clients[s_client_count++] = fd;
    }
    xSemaphoreGive(s_clients_lock);
}

static void uwl_ws_client_remove(int fd)
{
    if (!s_clients_lock) return;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_client_count; i++) {
        if (s_clients[i] == fd) {
            s_clients[i] = s_clients[s_client_count - 1];
            s_client_count--;
            break;
        }
    }
    xSemaphoreGive(s_clients_lock);
}

static esp_err_t uwl_ws_send_text_to_fd(int fd, const char *text)
{
    if (!s_server || !text) return ESP_ERR_INVALID_STATE;
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    return httpd_ws_send_frame_async(s_server, fd, &frame);
}

static void uwl_ws_broadcast_text(const char *text)
{
    if (!text || !s_clients_lock) return;

    int fds[8];
    size_t n = 0;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    n = s_client_count;
    if (n > 8) n = 8;
    for (size_t i = 0; i < n; i++) fds[i] = s_clients[i];
    xSemaphoreGive(s_clients_lock);

    for (size_t i = 0; i < n; i++) {
        const esp_err_t err = uwl_ws_send_text_to_fd(fds[i], text);
        if (err != ESP_OK) {
            // Common when client disconnects or session is purged: avoid log spam.
            if (err != ESP_ERR_INVALID_ARG && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "ws send failed fd=%d: %s", fds[i], esp_err_to_name(err));
            }
            uwl_ws_client_remove(fds[i]);
        }
    }
}

static void uwl_ws_status_task(void *arg)
{
    (void)arg;
    while (true) {
        const size_t clients = uwl_ws_get_client_count();
        if (clients > 0) {
            const int sta = uwl_wifi_softap_get_sta_count();
            const bool ble_conn = uwl_ble_is_connected();
            const bool ble_notify = uwl_ble_is_state_notify_enabled();

            char buf[192];
            const int n = snprintf(buf, sizeof(buf),
                                   "{\"type\":\"status\",\"sta_count\":%d,\"ws_clients\":%u,\"ble_connected\":%s,\"ble_notify\":%s}",
                                   sta,
                                   (unsigned)clients,
                                   ble_conn ? "true" : "false",
                                   ble_notify ? "true" : "false");
            if (n > 0 && (size_t)n < sizeof(buf)) {
                uwl_ws_broadcast_text(buf);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static char *uwl_build_state_json(void)
{
    size_t count = 0;
    const uwl_io_entry_t *entries = uwl_io_state_entries(&count);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "state");
    cJSON *arr = cJSON_AddArrayToObject(root, "gpios");

    for (size_t i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "pin", entries[i].pin);
        cJSON_AddStringToObject(o, "dir", entries[i].dir == UWL_IO_DIR_OUTPUT ? "out" : "in");
        cJSON_AddNumberToObject(o, "value", entries[i].value ? 1 : 0);
        cJSON_AddItemToArray(arr, o);
    }

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static char *uwl_build_gpio_changed_json(const uwl_io_event_t *evt)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "gpio_changed");
    cJSON_AddNumberToObject(root, "pin", evt->pin);
    cJSON_AddNumberToObject(root, "value", evt->value ? 1 : 0);
    cJSON_AddStringToObject(root, "dir", evt->dir == UWL_IO_DIR_OUTPUT ? "out" : "in");
    cJSON_AddStringToObject(root, "reason", evt->reason == UWL_IO_REASON_INPUT_EDGE ? "edge" :
                                        evt->reason == UWL_IO_REASON_SET_CMD ? "set" : "boot");
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static const char *uwl_err_code_from_esp(esp_err_t e)
{
    if (e == ESP_OK) return "OK";
    if (e == ESP_ERR_NOT_FOUND) return "NOT_FOUND";
    if (e == ESP_ERR_INVALID_STATE) return "NOT_OUTPUT";
    if (e == ESP_ERR_INVALID_ARG) return "BAD_ARG";
    if (e == ESP_ERR_NO_MEM) return "NO_MEM";
    if (e == ESP_ERR_NOT_SUPPORTED) return "NOT_SUPPORTED";
    return "FAIL";
}

static const char *uwl_json_get_str2(const cJSON *root, const char *k1, const char *k2)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, k1);
    if (cJSON_IsString(a) && a->valuestring) return a->valuestring;
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(root, k2);
    if (cJSON_IsString(b) && b->valuestring) return b->valuestring;
    return NULL;
}

static int uwl_json_get_i32_2(const cJSON *root, const char *k1, const char *k2, int defv)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, k1);
    if (cJSON_IsNumber(a)) return a->valueint;
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(root, k2);
    if (cJSON_IsNumber(b)) return b->valueint;
    return defv;
}

static void uwl_ws_send_err_to_fd(int fd, int id, const char *code, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "type", "err");
    if (id >= 0) cJSON_AddNumberToObject(o, "id", id);
    if (code) cJSON_AddStringToObject(o, "code", code);
    if (msg) cJSON_AddStringToObject(o, "msg", msg);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        (void)uwl_ws_send_text_to_fd(fd, s);
        cJSON_free(s);
    }
}

static void uwl_ws_send_resp_ok_to_fd(int fd, int id, cJSON *data_opt)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        if (data_opt) cJSON_Delete(data_opt);
        return;
    }
    cJSON_AddStringToObject(o, "type", "resp");
    if (id >= 0) cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddBoolToObject(o, "ok", true);
    if (data_opt) cJSON_AddItemToObject(o, "data", data_opt);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        (void)uwl_ws_send_text_to_fd(fd, s);
        cJSON_free(s);
    }
}

static void uwl_ws_on_io_event(const uwl_io_event_t *evt, void *ctx)
{
    (void)ctx;
    if (!evt) return;

    char *msg = uwl_build_gpio_changed_json(evt);
    if (!msg) return;
    uwl_ws_broadcast_text(msg);
    cJSON_free(msg);
}

static esp_err_t uwl_ws_handle_message(httpd_req_t *req, const char *payload, size_t len)
{
    (void)len;
    cJSON *root = cJSON_Parse(payload);
    if (!root) return ESP_ERR_INVALID_ARG;

    const int fd = httpd_req_to_sockfd(req);
    const char *type = uwl_json_get_str2(root, "type", "t");
    const int id = uwl_json_get_i32_2(root, "id", "i", -1);
    const int pin = uwl_json_get_i32_2(root, "pin", "p", -1);
    const int value = uwl_json_get_i32_2(root, "value", "v", 0);
    if (!type) {
        uwl_ws_send_err_to_fd(fd, id, "BAD_CMD", "missing type");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    // set (v1/v2)
    if (strcmp(type, "gpio_set") == 0 || strcmp(type, "s") == 0 || strcmp(type, "set") == 0) {
        if (pin < 0) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            err = uwl_io_state_set(pin, (uint8_t)(value ? 1 : 0), UWL_IO_SOURCE_WIFI);
        }
        if (err == ESP_OK) {
            cJSON *data = cJSON_CreateObject();
            if (data) {
                cJSON_AddNumberToObject(data, "pin", pin);
                cJSON_AddNumberToObject(data, "value", value ? 1 : 0);
            }
            uwl_ws_send_resp_ok_to_fd(fd, id, data);
        } else {
            uwl_ws_send_err_to_fd(fd, id, uwl_err_code_from_esp(err), "gpio_set failed");
        }
    }
    // get (v2 addition for Wi-Fi parity with BLE)
    else if (strcmp(type, "gpio_get") == 0 || strcmp(type, "g") == 0 || strcmp(type, "get") == 0) {
        if (pin < 0) {
            err = ESP_ERR_INVALID_ARG;
            uwl_ws_send_err_to_fd(fd, id, "BAD_ARG", "missing pin");
        } else {
            uint8_t v = 0;
            err = uwl_io_state_get(pin, &v);
            if (err == ESP_OK) {
                // Keep legacy "gpio" response too
                cJSON *o = cJSON_CreateObject();
                if (o) {
                    cJSON_AddStringToObject(o, "type", "gpio");
                    cJSON_AddNumberToObject(o, "pin", pin);
                    cJSON_AddNumberToObject(o, "value", v ? 1 : 0);
                    if (id >= 0) cJSON_AddNumberToObject(o, "id", id);
                    char *s = cJSON_PrintUnformatted(o);
                    cJSON_Delete(o);
                    if (s) {
                        (void)uwl_ws_send_text_to_fd(fd, s);
                        cJSON_free(s);
                    }
                }
                uwl_ws_send_resp_ok_to_fd(fd, id, NULL);
            } else {
                uwl_ws_send_err_to_fd(fd, id, uwl_err_code_from_esp(err), "gpio_get failed");
            }
        }
    }
    // list/state -> respond with state snapshot (as before), plus optional ACK
    else if (strcmp(type, "gpio_list") == 0 || strcmp(type, "l") == 0 || strcmp(type, "list") == 0 ||
             strcmp(type, "state") == 0) {
        char *state = uwl_build_state_json();
        if (state) {
            (void)uwl_ws_send_text_to_fd(fd, state);
            cJSON_free(state);
            uwl_ws_send_resp_ok_to_fd(fd, id, NULL);
            err = ESP_OK;
        } else {
            err = ESP_ERR_NO_MEM;
            uwl_ws_send_err_to_fd(fd, id, "NO_MEM", "no mem");
        }
    }
    else {
        err = ESP_ERR_NOT_SUPPORTED;
        uwl_ws_send_err_to_fd(fd, id, "NOT_SUPPORTED", "unknown type");
    }

    cJSON_Delete(root);
    return err;
}

static esp_err_t uwl_ws_handler(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        uwl_ws_client_add(fd);

        char *state = uwl_build_state_json();
        if (state) {
            (void)uwl_ws_send_text_to_fd(fd, state);
            cJSON_free(state);
        }
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = { 0 };

    esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws recv header failed: %s", esp_err_to_name(err));
        uwl_ws_client_remove(fd);
        return err;
    }

    if (ws_pkt.len == 0) {
        if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            uwl_ws_client_remove(fd);
        }
        return ESP_OK;
    }

    char *buf = (char *)calloc(1, ws_pkt.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    ws_pkt.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (err == ESP_OK) {
        buf[ws_pkt.len] = '\0';
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            (void)uwl_ws_handle_message(req, buf, ws_pkt.len);
        } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            uwl_ws_client_remove(fd);
        }
    } else {
        ESP_LOGW(TAG, "ws recv payload failed: %s", esp_err_to_name(err));
        uwl_ws_client_remove(fd);
    }

    free(buf);
    return ESP_OK;
}

esp_err_t uwl_ws_register(httpd_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    s_server = server;
    if (!s_clients_lock) {
        s_clients_lock = xSemaphoreCreateMutex();
    }

    // Register event listener once (idempotent enough for this app)
    (void)uwl_io_state_add_listener(uwl_ws_on_io_event, NULL);

    if (!s_status_task_started) {
        s_status_task_started = true;
        xTaskCreate(uwl_ws_status_task, "uwl_ws_stat", 3072, NULL, 6, NULL);
    }

    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = uwl_ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL,
    };

    return httpd_register_uri_handler(server, &ws);
}

