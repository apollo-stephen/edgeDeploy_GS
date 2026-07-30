#include "wifi_ap.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

#define WIFI_AP_SSID CONFIG_ESP_WIFI_SSID
#define WIFI_AP_PASSWORD CONFIG_ESP_WIFI_PASSWORD
#define WIFI_AP_CHANNEL CONFIG_ESP_WIFI_CHANNEL
#define WIFI_AP_MAX_CONNECTIONS CONFIG_ESP_MAX_STA_CONN

#if CONFIG_ESP_GTK_REKEYING_ENABLE
#define WIFI_AP_GTK_REKEY_INTERVAL CONFIG_ESP_GTK_REKEY_INTERVAL
#else
#define WIFI_AP_GTK_REKEY_INTERVAL 0
#endif

static const char *TAG = "wifi_ap";
static esp_netif_t *s_ap_netif;
static char s_ap_ip[16] = "";
static bool s_initialized;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG,
                 "Station " MACSTR " joined, AID=%d",
                 MAC2STR(event->mac),
                 event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG,
                 "Station " MACSTR " left, AID=%d, reason=%d",
                 MAC2STR(event->mac),
                 event->aid,
                 event->reason);
    }
}

static void cleanup_partial_init(esp_netif_t *ap_netif,
                                 bool event_loop_created,
                                 bool wifi_initialized,
                                 bool event_handler_registered,
                                 bool wifi_started)
{
    esp_err_t cleanup_err;

    if (wifi_started) {
        cleanup_err = esp_wifi_stop();
        if (cleanup_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Wi-Fi cleanup stop failed: %s",
                     esp_err_to_name(cleanup_err));
        }
    }

    if (event_handler_registered) {
        cleanup_err = esp_event_handler_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
        if (cleanup_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Wi-Fi event cleanup failed: %s",
                     esp_err_to_name(cleanup_err));
        }
    }

    if (ap_netif != NULL) {
        esp_netif_destroy_default_wifi(ap_netif);
    }

    if (wifi_initialized) {
        cleanup_err = esp_wifi_deinit();
        if (cleanup_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Wi-Fi driver cleanup failed: %s",
                     esp_err_to_name(cleanup_err));
        }
    }

    if (event_loop_created) {
        cleanup_err = esp_event_loop_delete_default();
        if (cleanup_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Default event loop cleanup failed: %s",
                     esp_err_to_name(cleanup_err));
        }
    }

    s_ap_ip[0] = '\0';
}

esp_err_t wifi_ap_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    bool event_loop_created = false;
    bool wifi_initialized = false;
    bool event_handler_registered = false;
    bool wifi_started = false;
    esp_netif_t *ap_netif = NULL;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        event_loop_created = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG,
                 "Default event loop creation failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create the SoftAP network interface");
        err = ESP_FAIL;
        goto fail;
    }

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        goto fail;
    }
    wifi_initialized = true;

    err = esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Wi-Fi event registration failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }
    event_handler_registered = true;

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .password = WIFI_AP_PASSWORD,
            .max_connection = WIFI_AP_MAX_CONNECTIONS,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg = {
                .required = true,
            },
#ifdef CONFIG_ESP_WIFI_BSS_MAX_IDLE_SUPPORT
            .bss_max_idle_cfg = {
                .period = WIFI_AP_DEFAULT_MAX_IDLE_PERIOD,
                .protected_keep_alive = 1,
            },
#endif
            .gtk_rekey_interval = WIFI_AP_GTK_REKEY_INTERVAL,
        },
    };

    if (strlen(WIFI_AP_PASSWORD) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Setting SoftAP mode failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Applying SoftAP configuration failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Starting SoftAP failed: %s", esp_err_to_name(err));
        goto fail;
    }
    wifi_started = true;

    esp_netif_ip_info_t ip_info;
    err = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Reading SoftAP IP failed: %s",
                 esp_err_to_name(err));
        goto fail;
    }

    snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ip_info.ip));
    s_ap_netif = ap_netif;
    s_initialized = true;

    ESP_LOGI(TAG,
             "WiFiAP ready: SSID=%s, channel=%d, IP=%s",
             WIFI_AP_SSID,
             WIFI_AP_CHANNEL,
             s_ap_ip);
    return ESP_OK;

fail:
    cleanup_partial_init(ap_netif,
                         event_loop_created,
                         wifi_initialized,
                         event_handler_registered,
                         wifi_started);
    return err;
}

const char *wifi_ap_get_ip(void)
{
    return s_ap_ip;
}
