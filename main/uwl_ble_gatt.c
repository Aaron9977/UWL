#include "uwl_ble_gatt.h"

#include "sdkconfig.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED) && CONFIG_BT_NIMBLE_ENABLED

#include <string.h>
#include <ctype.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "uwl_io_state.h"

static const char *TAG = "uwl_ble";

// 128-bit UUIDs (random)
static const ble_uuid128_t UWL_SVC_UUID =
    BLE_UUID128_INIT(0x5f,0x2f,0x1a,0x35,0x0d,0x2f,0x4f,0x0c,0x9a,0x1c,0x38,0x48,0x72,0x3b,0x8e,0x10);
static const ble_uuid128_t UWL_CTRL_UUID =
    BLE_UUID128_INIT(0x5f,0x2f,0x1a,0x35,0x0d,0x2f,0x4f,0x0c,0x9a,0x1c,0x38,0x48,0x72,0x3b,0x8e,0x11);
static const ble_uuid128_t UWL_STATE_UUID =
    BLE_UUID128_INIT(0x5f,0x2f,0x1a,0x35,0x0d,0x2f,0x4f,0x0c,0x9a,0x1c,0x38,0x48,0x72,0x3b,0x8e,0x12);

static uint16_t s_state_chr_val_handle = 0;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_state_notify_enabled = false;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;

static void uwl_ble_advertise_start(void);

bool uwl_ble_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool uwl_ble_is_state_notify_enabled(void)
{
    return s_state_notify_enabled;
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

static void uwl_ble_notify_text(const char *text)
{
    if (!text) return;
    if (!s_state_notify_enabled) return;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (s_state_chr_val_handle == 0) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, strlen(text));
    if (!om) return;
    (void)ble_gatts_notify_custom(s_conn_handle, s_state_chr_val_handle, om);
}

static const char *uwl_err_code_from_esp(esp_err_t e)
{
    if (e == ESP_OK) return "OK";
    if (e == ESP_ERR_NOT_FOUND) return "NOT_FOUND";
    if (e == ESP_ERR_INVALID_STATE) return "NOT_OUTPUT";
    if (e == ESP_ERR_INVALID_ARG) return "BAD_ARG";
    if (e == ESP_ERR_NO_MEM) return "NO_MEM";
    return "FAIL";
}

static const char *uwl_skip_ws(const char *s)
{
    while (s && *s && isspace((unsigned char)*s)) s++;
    return s;
}

static void uwl_ble_notify_err(int id, const char *code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "err");
    if (id >= 0) cJSON_AddNumberToObject(root, "id", id);
    if (code) cJSON_AddStringToObject(root, "code", code);
    if (msg) cJSON_AddStringToObject(root, "msg", msg);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) {
        uwl_ble_notify_text(s);
        cJSON_free(s);
    }
}

static void uwl_ble_notify_resp_ok(int id, cJSON *data_opt)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        if (data_opt) cJSON_Delete(data_opt);
        return;
    }
    cJSON_AddStringToObject(root, "type", "resp");
    if (id >= 0) cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", true);
    if (data_opt) cJSON_AddItemToObject(root, "data", data_opt);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) {
        uwl_ble_notify_text(s);
        cJSON_free(s);
    }
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

static void uwl_ble_cmd_state_snapshot_notify(int id)
{
    // For v2 clients, prefer READ of STATE characteristic for large payload stability.
    // We still support legacy behavior (notify full snapshot) when id is not provided.
    if (id >= 0) {
        cJSON *data = cJSON_CreateObject();
        if (data) {
            cJSON_AddStringToObject(data, "hint", "read_state_char");
            uwl_ble_notify_resp_ok(id, data);
        } else {
            uwl_ble_notify_resp_ok(id, NULL);
        }
        return;
    }

    char *state = uwl_build_state_json();
    if (state) {
        uwl_ble_notify_text(state);
        cJSON_free(state);
    }
}

static void uwl_ble_cmd_gpio_get_notify(int pin, int id)
{
    uint8_t v = 0;
    const esp_err_t err = uwl_io_state_get(pin, &v);
    if (err != ESP_OK) {
        uwl_ble_notify_err(id, uwl_err_code_from_esp(err), "gpio_get failed");
        return;
    }

    // Legacy response type (kept for compatibility)
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        uwl_ble_notify_err(id, "NO_MEM", "no mem");
        return;
    }
    cJSON_AddStringToObject(o, "type", "gpio");
    cJSON_AddNumberToObject(o, "pin", pin);
    cJSON_AddNumberToObject(o, "value", v ? 1 : 0);
    if (id >= 0) cJSON_AddNumberToObject(o, "id", id);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        uwl_ble_notify_text(s);
        cJSON_free(s);
    }
}

static void uwl_ble_cmd_gpio_set_ack(int pin, int value, int id)
{
    const esp_err_t err = uwl_io_state_set(pin, value ? 1 : 0, UWL_IO_SOURCE_BLE);
    if (err != ESP_OK) {
        uwl_ble_notify_err(id, uwl_err_code_from_esp(err), "gpio_set failed");
        return;
    }
    cJSON *data = cJSON_CreateObject();
    if (data) {
        cJSON_AddNumberToObject(data, "pin", pin);
        cJSON_AddNumberToObject(data, "value", value ? 1 : 0);
    }
    uwl_ble_notify_resp_ok(id, data);
}

static void uwl_ble_handle_text_cmd(const char *text)
{
    // Text protocol for easy manual use (e.g., nRF Connect):
    // s <pin> <0|1>
    // g <pin>
    // l
    // state
    char op[16] = { 0 };
    int p = 0, v = 0;
    const char *t = uwl_skip_ws(text);
    if (!t || !*t) return;

    // parse first token
    int n = sscanf(t, "%15s %d %d", op, &p, &v);
    if (n <= 0) {
        uwl_ble_notify_err(-1, "BAD_CMD", "empty");
        return;
    }
    if ((strcmp(op, "s") == 0 || strcmp(op, "set") == 0) && n >= 3) {
        uwl_ble_cmd_gpio_set_ack(p, v ? 1 : 0, -1);
        return;
    }
    if ((strcmp(op, "g") == 0 || strcmp(op, "get") == 0) && n >= 2) {
        uwl_ble_cmd_gpio_get_notify(p, -1);
        return;
    }
    if (strcmp(op, "l") == 0 || strcmp(op, "list") == 0) {
        uwl_ble_cmd_state_snapshot_notify(-1);
        return;
    }
    if (strcmp(op, "state") == 0) {
        uwl_ble_cmd_state_snapshot_notify(-1);
        return;
    }

    uwl_ble_notify_err(-1, "BAD_CMD", "use: s <pin> <0|1> | g <pin> | l | state");
}

static void uwl_ble_on_io_event(const uwl_io_event_t *evt, void *ctx)
{
    (void)ctx;
    if (!evt) return;
    char *msg = uwl_build_gpio_changed_json(evt);
    if (!msg) return;
    uwl_ble_notify_text(msg);
    cJSON_free(msg);
}

static int uwl_gatt_access_cb(uint16_t conn_handle,
                              uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt,
                              void *arg)
{
    (void)attr_handle;
    (void)arg;

    // CTRL characteristic: write JSON command.
    // Commands are compatible with WebSocket protocol:
    // - {"type":"gpio_set","pin":X,"value":0|1}
    // - {"type":"gpio_get","pin":X}
    // - {"type":"gpio_list"}
    // - {"type":"state"}
    //
    // v2 short-form (recommended):
    // - {"t":"s","p":X,"v":0|1,"i":id}
    // - {"t":"g","p":X,"i":id}
    // - {"t":"l","i":id}
    // - {"t":"state","i":id}  (prefer STATE characteristic read for full payload)
    //
    // Text form (manual tools):
    // - "s 18 1" / "g 18" / "l" / "state"
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        char *buf = (char *)calloc(1, len + 1);
        if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;
        os_mbuf_copydata(ctxt->om, 0, len, buf);
        buf[len] = '\0';

        const char *t0 = uwl_skip_ws(buf);
        if (!t0) {
            free(buf);
            return 0;
        }

        // Text protocol
        if (*t0 != '{') {
            uwl_ble_handle_text_cmd(t0);
            free(buf);
            return 0;
        }

        // JSON protocol
        cJSON *root = cJSON_Parse(t0);
        free(buf);
        if (!root) {
            uwl_ble_notify_err(-1, "BAD_JSON", "parse failed");
            return 0;
        }

        const char *type = uwl_json_get_str2(root, "type", "t");
        const int id = uwl_json_get_i32_2(root, "id", "i", -1);
        const int pin = uwl_json_get_i32_2(root, "pin", "p", -1);
        const int value = uwl_json_get_i32_2(root, "value", "v", 0);

        if (type) {
            // set
            if (strcmp(type, "gpio_set") == 0 || strcmp(type, "s") == 0 || strcmp(type, "set") == 0) {
                if (pin < 0) {
                    uwl_ble_notify_err(id, "BAD_ARG", "missing pin");
                } else {
                    uwl_ble_cmd_gpio_set_ack(pin, value ? 1 : 0, id);
                }
            }
            // get
            else if (strcmp(type, "gpio_get") == 0 || strcmp(type, "g") == 0 || strcmp(type, "get") == 0) {
                if (pin < 0) {
                    uwl_ble_notify_err(id, "BAD_ARG", "missing pin");
                } else {
                    uwl_ble_cmd_gpio_get_notify(pin, id);
                    if (id >= 0) uwl_ble_notify_resp_ok(id, NULL);
                }
            }
            // list/state
            else if (strcmp(type, "gpio_list") == 0 || strcmp(type, "l") == 0 ||
                     strcmp(type, "state") == 0) {
                uwl_ble_cmd_state_snapshot_notify(id);
            }
            else {
                uwl_ble_notify_err(id, "BAD_CMD", "unknown type");
            }
        } else {
            uwl_ble_notify_err(id, "BAD_CMD", "missing type");
        }

        cJSON_Delete(root);
        return 0;
    }

    // STATE characteristic: read returns full snapshot JSON
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char *state = uwl_build_state_json();
        if (!state) return BLE_ATT_ERR_INSUFFICIENT_RES;
        const int rc = os_mbuf_append(ctxt->om, state, strlen(state));
        cJSON_free(state);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UWL_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &UWL_CTRL_UUID.u,
                .access_cb = uwl_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &UWL_STATE_UUID.u,
                .access_cb = uwl_gatt_access_cb,
                .val_handle = &s_state_chr_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 },
};

static int uwl_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected, conn_handle=%u", (unsigned)s_conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE connect failed (status=%d); restarting adv", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_state_notify_enabled = false;
            uwl_ble_advertise_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected (reason=%d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_state_notify_enabled = false;
        uwl_ble_advertise_start();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_state_chr_val_handle) {
            s_state_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "BLE subscribe state notify=%d", (int)s_state_notify_enabled);
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE adv complete (reason=%d); restarting adv", event->adv_complete.reason);
        uwl_ble_advertise_start();
        return 0;

    default:
        return 0;
    }
}

static void uwl_ble_advertise_start(void)
{
    struct ble_gap_adv_params adv_params = { 0 };
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields = { 0 };
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.uuids128 = (ble_uuid128_t *)&UWL_SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, uwl_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_start rc=%d (addr_type=%u)", rc, (unsigned)s_own_addr_type);
        return;
    }

    ESP_LOGI(TAG, "BLE advertising started");
}

static void uwl_on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    uwl_ble_advertise_start();
}

static void uwl_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t uwl_ble_gatt_start(void)
{
    static bool started = false;
    if (started) return ESP_OK;
    started = true;

    // ESP-IDF NimBLE examples rely on nimble_port_init() to initialize everything needed
    // (including controller transport). Keep the same flow for ESP32-C6.
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = uwl_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("UWL-ESP32C6");

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
        return ESP_FAIL;
    }

    // Notify on IO changes
    (void)uwl_io_state_add_listener(uwl_ble_on_io_event, NULL);

    nimble_port_freertos_init(uwl_host_task);
    ESP_LOGI(TAG, "BLE GATT started");
    return ESP_OK;
}

#else

esp_err_t uwl_ble_gatt_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool uwl_ble_is_connected(void)
{
    return false;
}

bool uwl_ble_is_state_notify_enabled(void)
{
    return false;
}

#endif

