#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "CAMERA.h"
#include "esp_err.h"
#include "http_capture.h"
#include "inference.h"
#include "nvs_flash.h"
#include "wifi_ap.h"

void app_main(void);

enum call_id {
    CALL_NVS_INIT = 1,
    CALL_NVS_ERASE,
    CALL_CAMERA_INIT,
    CALL_WIFI_INIT,
    CALL_INFERENCE_START,
    CALL_HTTP_START,
};

static enum call_id s_calls[12];
static size_t s_call_count;
static esp_err_t s_nvs_results[2] = {ESP_OK, ESP_OK};
static size_t s_nvs_result_index;
static esp_err_t s_erase_result = ESP_OK;
static esp_err_t s_camera_result = ESP_OK;
static esp_err_t s_wifi_result = ESP_OK;
static esp_err_t s_http_result = ESP_OK;
static esp_err_t s_inference_result = ESP_OK;

static void record(enum call_id call)
{
    assert(s_call_count < 12);
    s_calls[s_call_count++] = call;
}

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test-error";
}

esp_err_t nvs_flash_init(void)
{
    record(CALL_NVS_INIT);
    assert(s_nvs_result_index < 2);
    return s_nvs_results[s_nvs_result_index++];
}

esp_err_t nvs_flash_erase(void)
{
    record(CALL_NVS_ERASE);
    return s_erase_result;
}

esp_err_t camera_init(void)
{
    record(CALL_CAMERA_INIT);
    return s_camera_result;
}

esp_err_t wifi_ap_init(void)
{
    record(CALL_WIFI_INIT);
    return s_wifi_result;
}

esp_err_t http_capture_start(void)
{
    record(CALL_HTTP_START);
    return s_http_result;
}

esp_err_t inference_start(void)
{
    record(CALL_INFERENCE_START);
    return s_inference_result;
}

const char *wifi_ap_get_ip(void)
{
    return "192.168.4.1";
}

static void verify_calls(const enum call_id *expected, size_t expected_count)
{
    assert(s_call_count == expected_count);
    for (size_t index = 0; index < expected_count; ++index) {
        assert(s_calls[index] == expected[index]);
    }
}

static void configure_scenario(const char *scenario)
{
    if (strcmp(scenario, "nvs-recovery") == 0) {
        s_nvs_results[0] = ESP_ERR_NVS_NO_FREE_PAGES;
        s_nvs_results[1] = ESP_OK;
    } else if (strcmp(scenario, "camera-failure") == 0) {
        s_camera_result = ESP_FAIL;
    } else if (strcmp(scenario, "wifi-failure") == 0) {
        s_wifi_result = ESP_FAIL;
    } else if (strcmp(scenario, "http-failure") == 0) {
        s_http_result = ESP_FAIL;
    } else if (strcmp(scenario, "inference-failure") == 0) {
        s_inference_result = ESP_FAIL;
    }
}

int main(int argc, char **argv)
{
    const char *scenario = argc == 2 ? argv[1] : "success";
    configure_scenario(scenario);
    app_main();

    if (strcmp(scenario, "success") == 0) {
        const enum call_id expected[] = {
            CALL_NVS_INIT,
            CALL_CAMERA_INIT,
            CALL_WIFI_INIT,
            CALL_INFERENCE_START,
            CALL_HTTP_START,
        };
        verify_calls(expected, sizeof(expected) / sizeof(expected[0]));
    } else if (strcmp(scenario, "nvs-recovery") == 0) {
        const enum call_id expected[] = {
            CALL_NVS_INIT,
            CALL_NVS_ERASE,
            CALL_NVS_INIT,
            CALL_CAMERA_INIT,
            CALL_WIFI_INIT,
            CALL_INFERENCE_START,
            CALL_HTTP_START,
        };
        verify_calls(expected, sizeof(expected) / sizeof(expected[0]));
    } else if (strcmp(scenario, "camera-failure") == 0) {
        const enum call_id expected[] = {
            CALL_NVS_INIT,
            CALL_CAMERA_INIT,
        };
        verify_calls(expected, sizeof(expected) / sizeof(expected[0]));
    } else if (strcmp(scenario, "wifi-failure") == 0) {
        const enum call_id expected[] = {
            CALL_NVS_INIT,
            CALL_CAMERA_INIT,
            CALL_WIFI_INIT,
        };
        verify_calls(expected, sizeof(expected) / sizeof(expected[0]));
    } else if (strcmp(scenario, "http-failure") == 0) {
        const enum call_id expected[] = {
            CALL_NVS_INIT,
            CALL_CAMERA_INIT,
            CALL_WIFI_INIT,
            CALL_INFERENCE_START,
            CALL_HTTP_START,
        };
        verify_calls(expected, sizeof(expected) / sizeof(expected[0]));
    } else if (strcmp(scenario, "inference-failure") == 0) {
        const enum call_id expected[] = {
            CALL_NVS_INIT,
            CALL_CAMERA_INIT,
            CALL_WIFI_INIT,
            CALL_INFERENCE_START,
        };
        verify_calls(expected, sizeof(expected) / sizeof(expected[0]));
    } else {
        assert(!"unknown scenario");
    }

    puts("http image transfer startup behavior passed");
    return 0;
}
