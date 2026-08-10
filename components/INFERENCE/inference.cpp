#include "inference.h"

#include <cstring>
#include <stddef.h>
#include <stdint.h>

#include "CAMERA.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/image/processing.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "img_converters.h"

namespace {

constexpr uint32_t kCaptureTimeoutMs = 250;
constexpr uint32_t kInferencePeriodMs = 2000;
constexpr configSTACK_DEPTH_TYPE kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 5;
constexpr BaseType_t kTaskCoreId = 1;
constexpr size_t kCaptureRgbBufferBytes =
    CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 3U;
constexpr size_t kModelRgbBufferBytes =
    EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3U;
// FIT_SHORTEST first copies the cropped source into the destination and then
// resizes it in place, so the destination also serves as a crop workspace.
constexpr size_t kResizeWorkspaceBytes =
    kCaptureRgbBufferBytes > kModelRgbBufferBytes ? kCaptureRgbBufferBytes
                                                  : kModelRgbBufferBytes;

static_assert(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE ==
                  EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT,
              "Image signal length must match model dimensions");
static_assert(EI_CLASSIFIER_LABEL_COUNT <= INFERENCE_MAX_LABELS,
              "Model label count exceeds inference snapshot capacity");
static_assert(INFERENCE_MAX_JPEG_BYTES == CAMERA_MAX_JPEG_BYTES,
              "Inference snapshot must match camera JPEG capacity");

const char *const kUncertainLabel = "uncertain";
const char *const TAG = "inference";

uint8_t *s_capture_rgb_buffer;
uint8_t *s_model_rgb_buffer;
uint8_t *s_staging_jpeg;
uint8_t *s_published_jpeg;
SemaphoreHandle_t s_snapshot_mutex;
inference_snapshot_metadata_t s_metadata;
TaskHandle_t s_task_handle;
portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
inference_runtime_stats_t s_runtime_stats;
bool s_started;

void release_resources()
{
    if (s_published_jpeg != nullptr) {
        heap_caps_free(s_published_jpeg);
        s_published_jpeg = nullptr;
    }
    if (s_staging_jpeg != nullptr) {
        heap_caps_free(s_staging_jpeg);
        s_staging_jpeg = nullptr;
    }
    if (s_model_rgb_buffer != nullptr) {
        heap_caps_free(s_model_rgb_buffer);
        s_model_rgb_buffer = nullptr;
    }
    if (s_capture_rgb_buffer != nullptr) {
        heap_caps_free(s_capture_rgb_buffer);
        s_capture_rgb_buffer = nullptr;
    }
    if (s_snapshot_mutex != nullptr) {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = nullptr;
    }
    s_metadata = {};
    s_task_handle = nullptr;
    portENTER_CRITICAL(&s_runtime_lock);
    s_runtime_stats = {};
    portEXIT_CRITICAL(&s_runtime_lock);
}

bool copy_label(char destination[INFERENCE_LABEL_BYTES], const char *source)
{
    if (source == nullptr) {
        return false;
    }
    const size_t length = strlen(source);
    if (length >= INFERENCE_LABEL_BYTES) {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

int get_signal_data(size_t offset, size_t length, float *out_ptr)
{
    if (out_ptr == nullptr || s_model_rgb_buffer == nullptr ||
        offset > EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE ||
        length > EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - offset) {
        return -1;
    }

    for (size_t index = 0; index < length; ++index) {
        const size_t pixel_offset = (offset + index) * 3U;
        const uint32_t red = s_model_rgb_buffer[pixel_offset];
        const uint32_t green = s_model_rgb_buffer[pixel_offset + 1U];
        const uint32_t blue = s_model_rgb_buffer[pixel_offset + 2U];
        out_ptr[index] = static_cast<float>((red << 16U) |
                                            (green << 8U) |
                                            blue);
    }
    return 0;
}

bool frame_is_valid(const camera_fb_t *frame)
{
    return frame != nullptr && frame->buf != nullptr && frame->len > 0 &&
           frame->width == CAMERA_FRAME_WIDTH &&
           frame->height == CAMERA_FRAME_HEIGHT &&
           frame->format == PIXFORMAT_JPEG;
}

bool build_metadata(const ei_impulse_result_t &result,
                    size_t jpeg_bytes,
                    inference_snapshot_metadata_t &metadata)
{
    const char *best_label = kUncertainLabel;
    float best_value = 0.0f;
    for (size_t index = 0; index < EI_CLASSIFIER_LABEL_COUNT; ++index) {
        const ei_impulse_result_classification_t &classification =
            result.classification[index];
        if (!copy_label(metadata.scores[index].label,
                        classification.label)) {
            return false;
        }
        metadata.scores[index].value = classification.value;
        if (classification.value > best_value) {
            best_value = classification.value;
            best_label = classification.label;
        }
    }

    if (best_value < EI_CLASSIFIER_THRESHOLD) {
        best_label = kUncertainLabel;
    }

    if (!copy_label(metadata.prediction, best_label)) {
        return false;
    }
    metadata.ready = true;
    metadata.confidence = best_value;
    metadata.label_count = EI_CLASSIFIER_LABEL_COUNT;
    metadata.timing.dsp_ms = result.timing.dsp;
    metadata.timing.classification_ms = result.timing.classification;
    metadata.timing.anomaly_ms = result.timing.anomaly;
    metadata.published_ms =
        static_cast<uint64_t>(esp_timer_get_time()) / 1000U;
    metadata.jpeg_bytes = jpeg_bytes;
    return true;
}

void log_result(const inference_snapshot_metadata_t &metadata)
{
    ESP_LOGI(TAG,
             "Timing: DSP %d ms, classification %d ms, anomaly %d ms",
             metadata.timing.dsp_ms,
             metadata.timing.classification_ms,
             metadata.timing.anomaly_ms);

    for (size_t index = 0; index < metadata.label_count; ++index) {
        ESP_LOGI(TAG,
                 "%s: %.5f",
                 metadata.scores[index].label,
                 static_cast<double>(metadata.scores[index].value));
    }
    ESP_LOGI(TAG,
             "Prediction: %s (%.5f)",
             metadata.prediction,
             static_cast<double>(metadata.confidence));
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

    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (s_snapshot_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create inference snapshot mutex");
        return ESP_ERR_NO_MEM;
    }

    s_capture_rgb_buffer = static_cast<uint8_t *>(
        heap_caps_malloc(kCaptureRgbBufferBytes,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_capture_rgb_buffer == nullptr) {
        ESP_LOGE(TAG,
                 "Failed to allocate %u-byte capture RGB888 buffer in PSRAM",
                 static_cast<unsigned int>(kCaptureRgbBufferBytes));
        release_resources();
        return ESP_ERR_NO_MEM;
    }

    s_model_rgb_buffer = static_cast<uint8_t *>(
        heap_caps_malloc(kResizeWorkspaceBytes,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_model_rgb_buffer == nullptr) {
        ESP_LOGE(TAG,
                 "Failed to allocate %u-byte resize workspace in PSRAM",
                 static_cast<unsigned int>(kResizeWorkspaceBytes));
        release_resources();
        return ESP_ERR_NO_MEM;
    }

    s_staging_jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(INFERENCE_MAX_JPEG_BYTES,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_staging_jpeg == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate inference staging JPEG buffer");
        release_resources();
        return ESP_ERR_NO_MEM;
    }

    s_published_jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(INFERENCE_MAX_JPEG_BYTES,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_published_jpeg == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate published inference JPEG buffer");
        release_resources();
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t task_result = xTaskCreatePinnedToCore(inference_task,
                                                           "ei_inference",
                                                           kTaskStackBytes,
                                                           nullptr,
                                                           kTaskPriority,
                                                           &s_task_handle,
                                                           kTaskCoreId);
    if (task_result != pdPASS) {
        release_resources();
        ESP_LOGE(TAG, "Failed to create inference task");
        return ESP_FAIL;
    }

    s_started = true;
    portENTER_CRITICAL(&s_runtime_lock);
    s_runtime_stats = {};
    s_runtime_stats.task_started = true;
    portEXIT_CRITICAL(&s_runtime_lock);
    ESP_LOGI(TAG,
             "Inference task started with %u ms period",
             static_cast<unsigned int>(kInferencePeriodMs));
    return ESP_OK;
}

static esp_err_t run_inference_attempt()
{
    camera_fb_t *frame = camera_capture_frame(kCaptureTimeoutMs);
    if (frame == nullptr) {
        ESP_LOGW(TAG, "Camera frame unavailable");
        return ESP_FAIL;
    }

    if (!frame_is_valid(frame)) {
        ESP_LOGE(TAG,
                 "Captured frame is not a valid %ux%u JPEG",
                 static_cast<unsigned int>(CAMERA_FRAME_WIDTH),
                 static_cast<unsigned int>(CAMERA_FRAME_HEIGHT));
        camera_release_frame(frame);
        return ESP_ERR_INVALID_ARG;
    }

    if (frame->len > INFERENCE_MAX_JPEG_BYTES) {
        ESP_LOGE(TAG,
                 "Captured JPEG exceeds %u-byte inference snapshot buffer",
                 static_cast<unsigned int>(INFERENCE_MAX_JPEG_BYTES));
        camera_release_frame(frame);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t jpeg_bytes = frame->len;
    memcpy(s_staging_jpeg, frame->buf, jpeg_bytes);

    const bool decoded = fmt2rgb888(frame->buf,
                                    frame->len,
                                    frame->format,
                                    s_capture_rgb_buffer);
    camera_release_frame(frame);
    if (!decoded) {
        ESP_LOGE(TAG, "JPEG to RGB888 conversion failed");
        return ESP_FAIL;
    }

    const int resize_result =
        ei::image::processing::resize_image_using_mode(
            s_capture_rgb_buffer,
            CAMERA_FRAME_WIDTH,
            CAMERA_FRAME_HEIGHT,
            s_model_rgb_buffer,
            EI_CLASSIFIER_INPUT_WIDTH,
            EI_CLASSIFIER_INPUT_HEIGHT,
            3,
            EI_CLASSIFIER_RESIZE_MODE);
    if (resize_result != 0) {
        ESP_LOGE(TAG,
                 "RGB resize %ux%u to %ux%u failed: %d",
                 static_cast<unsigned int>(CAMERA_FRAME_WIDTH),
                 static_cast<unsigned int>(CAMERA_FRAME_HEIGHT),
                 static_cast<unsigned int>(EI_CLASSIFIER_INPUT_WIDTH),
                 static_cast<unsigned int>(EI_CLASSIFIER_INPUT_HEIGHT),
                 resize_result);
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

    inference_snapshot_metadata_t next_metadata = {};
    if (!build_metadata(result, jpeg_bytes, next_metadata)) {
        ESP_LOGE(TAG, "Inference result label exceeds snapshot capacity");
        return ESP_ERR_INVALID_SIZE;
    }

    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock inference snapshot");
        return ESP_FAIL;
    }
    uint8_t *const previous_published = s_published_jpeg;
    s_published_jpeg = s_staging_jpeg;
    s_staging_jpeg = previous_published;
    next_metadata.sequence = s_metadata.sequence + 1U;
    if (next_metadata.sequence == 0U) {
        next_metadata.sequence = 1U;
    }
    s_metadata = next_metadata;
    xSemaphoreGive(s_snapshot_mutex);

    log_result(next_metadata);
    return ESP_OK;
}

extern "C" esp_err_t inference_run_once(void)
{
    if (!s_started || s_capture_rgb_buffer == nullptr ||
        s_model_rgb_buffer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t started_us =
        static_cast<uint64_t>(esp_timer_get_time());
    portENTER_CRITICAL(&s_runtime_lock);
    s_runtime_stats.attempt_running = true;
    ++s_runtime_stats.attempt_count;
    s_runtime_stats.last_attempt_started_us = started_us;
    portEXIT_CRITICAL(&s_runtime_lock);

    const esp_err_t result = run_inference_attempt();
    const uint64_t finished_us =
        static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t duration_us = finished_us >= started_us
                                     ? finished_us - started_us
                                     : 0U;

    portENTER_CRITICAL(&s_runtime_lock);
    s_runtime_stats.attempt_running = false;
    s_runtime_stats.last_attempt_finished_us = finished_us;
    s_runtime_stats.last_duration_us = duration_us;
    if (duration_us > s_runtime_stats.max_duration_us) {
        s_runtime_stats.max_duration_us = duration_us;
    }
    s_runtime_stats.last_error = result;
    if (result == ESP_OK) {
        ++s_runtime_stats.success_count;
        s_runtime_stats.consecutive_failure_count = 0;
        s_runtime_stats.last_success_us = finished_us;
    }
    else {
        ++s_runtime_stats.failure_count;
        ++s_runtime_stats.consecutive_failure_count;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    return result;
}

extern "C" esp_err_t inference_get_runtime_stats(
    inference_runtime_stats_t *stats)
{
    if (stats == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || s_task_handle == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    *stats = s_runtime_stats;
    portEXIT_CRITICAL(&s_runtime_lock);
    stats->stack_high_water_mark_bytes =
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_task_handle));
    return ESP_OK;
}

extern "C" esp_err_t inference_get_latest_metadata(
    inference_snapshot_metadata_t *metadata)
{
    if (metadata == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_snapshot_mutex == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (!s_metadata.ready) {
        xSemaphoreGive(s_snapshot_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *metadata = s_metadata;
    xSemaphoreGive(s_snapshot_mutex);
    return ESP_OK;
}

extern "C" esp_err_t inference_copy_latest_jpeg(
    uint32_t expected_sequence,
    uint8_t *destination,
    size_t capacity,
    size_t *jpeg_bytes)
{
    if (destination == nullptr || jpeg_bytes == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_snapshot_mutex == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (!s_metadata.ready) {
        xSemaphoreGive(s_snapshot_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_sequence != s_metadata.sequence) {
        xSemaphoreGive(s_snapshot_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (capacity < s_metadata.jpeg_bytes) {
        xSemaphoreGive(s_snapshot_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(destination, s_published_jpeg, s_metadata.jpeg_bytes);
    *jpeg_bytes = s_metadata.jpeg_bytes;
    xSemaphoreGive(s_snapshot_mutex);
    return ESP_OK;
}
