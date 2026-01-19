#include "uwl_wifi_softap.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "uwl_wifi_ap";
static volatile int s_sta_count = 0;

#ifndef CONFIG_UWL_WIFI_AP_SSID
#define CONFIG_UWL_WIFI_AP_SSID "UWL-ESP32C6"
#endif
#ifndef CONFIG_UWL_WIFI_AP_PASS
#define CONFIG_UWL_WIFI_AP_PASS "uwl123456"
#endif
#ifndef CONFIG_UWL_WIFI_AP_CHANNEL
#define CONFIG_UWL_WIFI_AP_CHANNEL 6
#endif
#ifndef CONFIG_UWL_WIFI_AP_MAX_CONN
#define CONFIG_UWL_WIFI_AP_MAX_CONN 4
#endif

static void uwl_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "SoftAP started, ssid=%s", CONFIG_UWL_WIFI_AP_SSID);
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            s_sta_count++;
            ESP_LOGI(TAG, "Station connected");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_sta_count > 0) s_sta_count--;
            ESP_LOGI(TAG, "Station disconnected");
            break;
        default:
            break;
        }
    }
}

int uwl_wifi_softap_get_sta_count(void)
{
    return (int)s_sta_count;
}

esp_err_t uwl_wifi_softap_start(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &uwl_wifi_event_handler, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.ap.ssid, CONFIG_UWL_WIFI_AP_SSID, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = (uint8_t)strlen(CONFIG_UWL_WIFI_AP_SSID);
    strncpy((char *)wifi_config.ap.password, CONFIG_UWL_WIFI_AP_PASS, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.channel = CONFIG_UWL_WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = CONFIG_UWL_WIFI_AP_MAX_CONN;
    wifi_config.ap.pmf_cfg.required = false;

    const size_t pass_len = strlen(CONFIG_UWL_WIFI_AP_PASS);
    if (pass_len == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    } else if (pass_len < 8) {
        ESP_LOGW(TAG, "SoftAP password too short (<8), falling back to OPEN");
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        // WPA/WPA2 mixed improves compatibility with some clients.
        wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Force 20MHz bandwidth for better client compatibility/stability.
    (void)esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);

    return ESP_OK;
}

