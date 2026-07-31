#include "CAMERA.h"
#include "esp_err.h"
#include "esp_log.h"
#include "http_capture.h"
#include "nvs_flash.h"
#include "wifi_ap.h"

static const char *TAG = "main";

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "NVS initialization failed: %s",
                 esp_err_to_name(err));
        return;
    }

    err = camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Camera initialization failed: %s",
                 esp_err_to_name(err));
        return;
    }

    err = wifi_ap_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "WiFiAP initialization failed: %s",
                 esp_err_to_name(err));
        return;
    }

    err = http_capture_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "HTTP capture startup failed: %s",
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG,
             "Image preview ready at http://%s/",
             wifi_ap_get_ip());
}
