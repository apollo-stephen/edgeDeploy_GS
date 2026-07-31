#include "CAMERA.h"

#include <limits.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "camera";
static SemaphoreHandle_t s_capture_mutex;
static bool s_camera_ready;

esp_err_t camera_init(void)
{
    if (s_camera_ready) {
        return ESP_OK;
    }

    s_capture_mutex = xSemaphoreCreateMutex();
    if (s_capture_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create capture mutex");
        return ESP_ERR_NO_MEM;
    }

    const camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,
        .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,
        .xclk_freq_hz = CAMERA_XCLK_FREQ_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_128X128,
        .jpeg_quality = CAMERA_JPEG_QUALITY,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    ESP_LOGI(TAG, "Initializing OV5640 in 128x128 JPEG capture mode");
    const esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Camera initialization failed: %s",
                 esp_err_to_name(err));
        vSemaphoreDelete(s_capture_mutex);
        s_capture_mutex = NULL;
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_pixformat(sensor, PIXFORMAT_JPEG);
        sensor->set_framesize(sensor, FRAMESIZE_128X128);
        sensor->set_hmirror(sensor, 1);
        sensor->set_vflip(sensor, 1);
        ESP_LOGI(TAG,
                 "Detected camera sensor PID: 0x%04x",
                 sensor->id.PID);
    } else {
        ESP_LOGW(TAG, "Camera initialized without a sensor handle");
    }

    s_camera_ready = true;
    ESP_LOGI(TAG,
             "Camera ready: %dx%d JPEG",
             CAMERA_FRAME_WIDTH,
             CAMERA_FRAME_HEIGHT);
    return ESP_OK;
}

camera_fb_t *camera_capture_frame(uint32_t timeout_ms)
{
    if (!s_camera_ready || s_capture_mutex == NULL) {
        return NULL;
    }

    const TickType_t timeout_ticks = timeout_ms == UINT32_MAX
                                         ? portMAX_DELAY
                                         : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_capture_mutex, timeout_ticks) != pdTRUE) {
        ESP_LOGW(TAG, "Camera is busy");
        return NULL;
    }

    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == NULL) {
        ESP_LOGE(TAG, "Failed to capture a frame");
        xSemaphoreGive(s_capture_mutex);
        return NULL;
    }

    return frame;
}

void camera_release_frame(camera_fb_t *frame)
{
    if (frame == NULL || s_capture_mutex == NULL) {
        return;
    }

    esp_camera_fb_return(frame);
    xSemaphoreGive(s_capture_mutex);
}

bool camera_is_ready(void)
{
    return s_camera_ready;
}

const char *camera_frame_size_name(void)
{
    return "128x128";
}
