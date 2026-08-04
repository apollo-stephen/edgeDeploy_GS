#include "inference.h"

#include <stddef.h>
#include <stdint.h>

#include "CAMERA.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"

namespace {

constexpr uint32_t kCaptureTimeoutMs = 250;
constexpr uint32_t kInferencePeriodMs = 2000;
constexpr configSTACK_DEPTH_TYPE kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 5;
constexpr BaseType_t kTaskCoreId = 1;
constexpr size_t kRgbBufferBytes =
    EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3U;

const char *const kUncertainLabel = "uncertain";
const char *const TAG = "inference";

uint8_t *s_rgb_buffer;
bool s_started;

int get_signal_data(size_t offset, size_t length, float *out_ptr)
{
    if (out_ptr == nullptr || s_rgb_buffer == nullptr ||
        offset > EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE ||
        length > EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - offset) {
        return -1;
    }

    for (size_t index = 0; index < length; ++index) {
        const size_t pixel_offset = (offset + index) * 3U;
        const uint32_t red = s_rgb_buffer[pixel_offset];
        const uint32_t green = s_rgb_buffer[pixel_offset + 1U];
        const uint32_t blue = s_rgb_buffer[pixel_offset + 2U];
        out_ptr[index] = static_cast<float>((red << 16U) |
                                            (green << 8U) |
                                            blue);
    }
    return 0;
}

bool frame_is_valid(const camera_fb_t *frame)
{
    return frame != nullptr && frame->buf != nullptr && frame->len > 0 &&
           frame->width == EI_CLASSIFIER_INPUT_WIDTH &&
           frame->height == EI_CLASSIFIER_INPUT_HEIGHT &&
           frame->format == PIXFORMAT_JPEG;
}

void log_result(const ei_impulse_result_t &result)
{
    ESP_LOGI(TAG,
             "Timing: DSP %d ms, classification %d ms, anomaly %d ms",
             result.timing.dsp,
             result.timing.classification,
             result.timing.anomaly);

    const char *best_label = kUncertainLabel;
    float best_value = 0.0f;
    for (size_t index = 0; index < EI_CLASSIFIER_LABEL_COUNT; ++index) {
        const ei_impulse_result_classification_t &classification =
            result.classification[index];
        ESP_LOGI(TAG,
                 "%s: %.5f",
                 classification.label,
                 static_cast<double>(classification.value));
        if (classification.value > best_value) {
            best_value = classification.value;
            best_label = classification.label;
        }
    }

    if (best_value < EI_CLASSIFIER_THRESHOLD) {
        best_label = kUncertainLabel;
    }
    ESP_LOGI(TAG,
             "Prediction: %s (%.5f)",
             best_label,
             static_cast<double>(best_value));
}

void inference_task(void *argument)
{
    (void)argument;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kInferencePeriodMs));
        const esp_err_t err = inference_run_once();
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Inference iteration failed: %s",
                     esp_err_to_name(err));
        }
    }
}

}  // namespace

extern "C" esp_err_t inference_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_rgb_buffer = static_cast<uint8_t *>(
        heap_caps_malloc(kRgbBufferBytes,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_rgb_buffer == nullptr) {
        ESP_LOGE(TAG,
                 "Failed to allocate %u-byte RGB888 buffer in PSRAM",
                 static_cast<unsigned int>(kRgbBufferBytes));
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t task_result = xTaskCreatePinnedToCore(inference_task,
                                                           "ei_inference",
                                                           kTaskStackBytes,
                                                           nullptr,
                                                           kTaskPriority,
                                                           nullptr,
                                                           kTaskCoreId);
    if (task_result != pdPASS) {
        heap_caps_free(s_rgb_buffer);
        s_rgb_buffer = nullptr;
        ESP_LOGE(TAG, "Failed to create inference task");
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG,
             "Inference task started with %u ms period",
             static_cast<unsigned int>(kInferencePeriodMs));
    return ESP_OK;
}

extern "C" esp_err_t inference_run_once(void)
{
    if (!s_started || s_rgb_buffer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    camera_fb_t *frame = camera_capture_frame(kCaptureTimeoutMs);
    if (frame == nullptr) {
        ESP_LOGW(TAG, "Camera frame unavailable");
        return ESP_FAIL;
    }

    if (!frame_is_valid(frame)) {
        ESP_LOGE(TAG, "Captured frame is not a valid 128x128 JPEG");
        camera_release_frame(frame);
        return ESP_ERR_INVALID_ARG;
    }

    const bool decoded = fmt2rgb888(frame->buf,
                                    frame->len,
                                    frame->format,
                                    s_rgb_buffer);
    camera_release_frame(frame);
    if (!decoded) {
        ESP_LOGE(TAG, "JPEG to RGB888 conversion failed");
        return ESP_FAIL;
    }

    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
    signal.get_data = get_signal_data;

    ei_impulse_result_t result = {};
    const EI_IMPULSE_ERROR classifier_result =
        run_classifier(&signal, &result, false);
    if (classifier_result != EI_IMPULSE_OK) {
        ESP_LOGE(TAG,
                 "Edge Impulse classifier failed: %d",
                 static_cast<int>(classifier_result));
        return ESP_FAIL;
    }

    log_result(result);
    return ESP_OK;
}
