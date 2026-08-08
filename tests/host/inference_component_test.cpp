#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "CAMERA.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/image/processing.hpp"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "inference.h"

struct fake_semaphore {
    int unused;
};

static uint8_t s_jpeg[] = {0xff, 0xd8, 0x11, 0x22, 0xff, 0xd9};
static camera_fb_t s_frame = {
    .buf = s_jpeg,
    .len = sizeof(s_jpeg),
    .width = 128,
    .height = 128,
    .format = PIXFORMAT_JPEG,
};
static camera_fb_t *s_next_frame = &s_frame;
static int s_capture_calls;
static int s_release_calls;
static uint32_t s_capture_timeout;
static bool s_decode_result = true;
static int s_decode_calls;
static uint8_t *s_decode_destination;
static int s_resize_calls;
static int s_resize_result;
static const uint8_t *s_resize_source;
static uint8_t *s_resize_destination;
static int s_resize_source_width;
static int s_resize_source_height;
static int s_resize_destination_width;
static int s_resize_destination_height;
static int s_resize_pixel_size;
static int s_resize_mode;
static int s_allocation_calls;
static int s_fail_allocation_call;
static std::vector<size_t> s_allocation_sizes;
static std::vector<unsigned int> s_allocation_caps;
static int s_free_calls;
static bool s_create_mutex = true;
static fake_semaphore s_mutex;
static int s_mutex_create_calls;
static int s_mutex_take_calls;
static int s_mutex_give_calls;
static int s_mutex_delete_calls;
static BaseType_t s_task_result = pdPASS;
static int s_task_create_calls;
static uint32_t s_task_stack_depth;
static UBaseType_t s_task_priority;
static BaseType_t s_task_core_id = -1;
static int s_classifier_calls;
static EI_IMPULSE_ERROR s_classifier_result = EI_IMPULSE_OK;
static float s_scores[EI_CLASSIFIER_LABEL_COUNT] = {0.05f, 0.90f, 0.05f};
static std::vector<uint32_t> s_observed_pixels;
static std::string s_logs;
static int64_t s_fake_time_us = 52825000;

extern "C" const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test-error";
}

extern "C" int64_t esp_timer_get_time(void)
{
    return s_fake_time_us;
}

extern "C" camera_fb_t *camera_capture_frame(uint32_t timeout_ms)
{
    ++s_capture_calls;
    s_capture_timeout = timeout_ms;
    return s_next_frame;
}

extern "C" void camera_release_frame(camera_fb_t *frame)
{
    assert(frame == &s_frame);
    ++s_release_calls;
}

extern "C" bool camera_is_ready(void)
{
    return true;
}

extern "C" bool fmt2rgb888(const uint8_t *source,
                            size_t source_len,
                            pixformat_t format,
                            uint8_t *destination)
{
    assert(source == s_jpeg);
    assert(source_len == s_frame.len);
    assert(format == PIXFORMAT_JPEG);
    ++s_decode_calls;
    if (!s_decode_result) {
        return false;
    }

    s_decode_destination = destination;
    destination[0] = 0x10;
    destination[1] = 0x20;
    destination[2] = 0x30;
    return true;
}

namespace ei { namespace image { namespace processing {
int resize_image_using_mode(const uint8_t *src_image,
                            int src_width,
                            int src_height,
                            uint8_t *dst_image,
                            int dst_width,
                            int dst_height,
                            int pixel_size_bytes,
                            int mode)
{
    ++s_resize_calls;
    s_resize_source = src_image;
    s_resize_destination = dst_image;
    s_resize_source_width = src_width;
    s_resize_source_height = src_height;
    s_resize_destination_width = dst_width;
    s_resize_destination_height = dst_height;
    s_resize_pixel_size = pixel_size_bytes;
    s_resize_mode = mode;
    assert(s_release_calls == s_decode_calls);
    if (s_resize_result != 0) {
        return s_resize_result;
    }

    memset(dst_image,
           0,
           EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3U);
    dst_image[0] = 0x40;
    dst_image[1] = 0x50;
    dst_image[2] = 0x60;
    return 0;
}
}}}

extern "C" void *heap_caps_malloc(size_t size, unsigned int capabilities)
{
    ++s_allocation_calls;
    s_allocation_sizes.push_back(size);
    s_allocation_caps.push_back(capabilities);
    if (s_allocation_calls == s_fail_allocation_call) {
        return nullptr;
    }
    return malloc(size);
}

extern "C" void heap_caps_free(void *pointer)
{
    ++s_free_calls;
    free(pointer);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    ++s_mutex_create_calls;
    return s_create_mutex ? &s_mutex : nullptr;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(semaphore == &s_mutex);
    assert(timeout == portMAX_DELAY);
    ++s_mutex_take_calls;
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_mutex);
    ++s_mutex_give_calls;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_mutex);
    ++s_mutex_delete_calls;
}

extern "C" BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task,
                                               const char *name,
                                               configSTACK_DEPTH_TYPE stack_depth,
                                               void *argument,
                                               UBaseType_t priority,
                                               TaskHandle_t *task_handle,
                                               BaseType_t core_id)
{
    assert(task != nullptr);
    assert(strcmp(name, "ei_inference") == 0);
    assert(argument == nullptr);
    assert(task_handle == nullptr);
    ++s_task_create_calls;
    s_task_stack_depth = stack_depth;
    s_task_priority = priority;
    s_task_core_id = core_id;
    return s_task_result;
}

extern "C" void vTaskDelay(TickType_t ticks)
{
    assert(ticks == pdMS_TO_TICKS(2000));
}

EI_IMPULSE_ERROR run_classifier(ei::signal_t *signal,
                                ei_impulse_result_t *result,
                                bool debug)
{
    assert(signal != nullptr);
    assert(signal->total_length == EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
    assert(!debug);
    ++s_classifier_calls;

    float pixels[2] = {};
    assert(signal->get_data(0, 2, pixels) == 0);
    s_observed_pixels.push_back(static_cast<uint32_t>(pixels[0]));
    s_observed_pixels.push_back(static_cast<uint32_t>(pixels[1]));

    result->timing.dsp = 11;
    result->timing.classification = 22;
    result->timing.anomaly = 0;
    for (size_t index = 0; index < EI_CLASSIFIER_LABEL_COUNT; ++index) {
        result->classification[index].label =
            ei_classifier_inferencing_categories[index];
        result->classification[index].value = s_scores[index];
    }
    return s_classifier_result;
}

extern "C" void test_log_write(const char *level,
                                const char *tag,
                                const char *format,
                                ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    s_logs += level;
    s_logs += ":";
    s_logs += tag;
    s_logs += ":";
    s_logs += message;
    s_logs += "\n";
}

static void verify_not_ready(void)
{
    inference_snapshot_metadata_t metadata = {};
    uint8_t jpeg[INFERENCE_MAX_JPEG_BYTES] = {};
    size_t jpeg_bytes = 0;
    assert(inference_get_latest_metadata(nullptr) == ESP_ERR_INVALID_ARG);
    assert(inference_get_latest_metadata(&metadata) == ESP_ERR_NOT_FOUND);
    assert(inference_copy_latest_jpeg(1,
                                      jpeg,
                                      sizeof(jpeg),
                                      &jpeg_bytes) == ESP_ERR_NOT_FOUND);
}

static void verify_start_success(void)
{
    verify_not_ready();
    assert(inference_run_once() == ESP_ERR_INVALID_STATE);
    assert(inference_start() == ESP_OK);
    assert(inference_start() == ESP_ERR_INVALID_STATE);
    assert(s_mutex_create_calls == 1);
    assert(s_task_create_calls == 1);
    assert(s_task_stack_depth == 8192);
    assert(s_task_priority == 5);
    assert(s_task_core_id == 1);
    assert(s_allocation_calls == 4);
    assert(s_allocation_sizes[0] == CAMERA_FRAME_WIDTH *
                                        CAMERA_FRAME_HEIGHT * 3U);
    assert(s_allocation_sizes[1] == EI_CLASSIFIER_INPUT_WIDTH *
                                        EI_CLASSIFIER_INPUT_HEIGHT * 3U);
    assert(s_allocation_sizes[2] == INFERENCE_MAX_JPEG_BYTES);
    assert(s_allocation_sizes[3] == INFERENCE_MAX_JPEG_BYTES);
    for (unsigned int capabilities : s_allocation_caps) {
        assert((capabilities & MALLOC_CAP_SPIRAM) != 0);
        assert((capabilities & MALLOC_CAP_8BIT) != 0);
    }
}

static inference_snapshot_metadata_t get_metadata(void)
{
    inference_snapshot_metadata_t metadata = {};
    assert(inference_get_latest_metadata(&metadata) == ESP_OK);
    return metadata;
}

static void verify_published_jpeg(uint32_t sequence)
{
    uint8_t jpeg_copy[INFERENCE_MAX_JPEG_BYTES] = {};
    size_t jpeg_bytes = 0;
    assert(inference_copy_latest_jpeg(sequence,
                                      jpeg_copy,
                                      sizeof(jpeg_copy),
                                      &jpeg_bytes) == ESP_OK);
    assert(jpeg_bytes == sizeof(s_jpeg));
    assert(memcmp(jpeg_copy, s_jpeg, jpeg_bytes) == 0);
}

static void verify_first_success(void)
{
    assert(inference_run_once() == ESP_OK);
    const inference_snapshot_metadata_t metadata = get_metadata();
    assert(metadata.ready);
    assert(metadata.sequence == 1);
    assert(metadata.label_count == 3);
    assert(strcmp(metadata.prediction, "recycleable") == 0);
    assert(metadata.confidence == 0.90f);
    assert(metadata.timing.dsp_ms == 11);
    assert(metadata.timing.classification_ms == 22);
    assert(metadata.timing.anomaly_ms == 0);
    assert(metadata.published_ms == 52825);
    assert(metadata.jpeg_bytes == sizeof(s_jpeg));
    assert(strcmp(metadata.scores[0].label, "harmful") == 0);
    assert(metadata.scores[0].value == 0.05f);
    assert(strcmp(metadata.scores[1].label, "recycleable") == 0);
    assert(metadata.scores[1].value == 0.90f);
    assert(strcmp(metadata.scores[2].label, "wet") == 0);
    assert(metadata.scores[2].value == 0.05f);
    verify_published_jpeg(metadata.sequence);
}

static void verify_success(void)
{
    verify_start_success();
    verify_first_success();
    assert(s_capture_calls == 1);
    assert(s_capture_timeout == 250);
    assert(s_release_calls == 1);
    assert(s_decode_calls == 1);
    assert(s_decode_destination == s_resize_source);
    assert(s_resize_source != s_resize_destination);
    assert(s_resize_calls == 1);
    assert(s_resize_source_width == CAMERA_FRAME_WIDTH);
    assert(s_resize_source_height == CAMERA_FRAME_HEIGHT);
    assert(s_resize_destination_width == EI_CLASSIFIER_INPUT_WIDTH);
    assert(s_resize_destination_height == EI_CLASSIFIER_INPUT_HEIGHT);
    assert(s_resize_pixel_size == 3);
    assert(s_resize_mode == EI_CLASSIFIER_RESIZE_MODE);
    assert(s_classifier_calls == 1);
    assert(s_observed_pixels.size() == 2);
    assert(s_observed_pixels[0] == 0x405060);
    assert(s_observed_pixels[1] == 0);
    assert(s_logs.find("recycleable") != std::string::npos);
    assert(s_logs.find("0.90000") != std::string::npos);

    uint8_t jpeg_copy[INFERENCE_MAX_JPEG_BYTES] = {};
    size_t jpeg_bytes = 0;
    assert(inference_copy_latest_jpeg(1, nullptr, 0, &jpeg_bytes) ==
           ESP_ERR_INVALID_ARG);
    assert(inference_copy_latest_jpeg(1,
                                      jpeg_copy,
                                      sizeof(jpeg_copy),
                                      nullptr) == ESP_ERR_INVALID_ARG);
    assert(inference_copy_latest_jpeg(1,
                                      jpeg_copy,
                                      sizeof(s_jpeg) - 1,
                                      &jpeg_bytes) == ESP_ERR_INVALID_SIZE);

    assert(inference_run_once() == ESP_OK);
    assert(get_metadata().sequence == 2);
    assert(inference_copy_latest_jpeg(1,
                                      jpeg_copy,
                                      sizeof(jpeg_copy),
                                      &jpeg_bytes) == ESP_ERR_INVALID_STATE);
    verify_published_jpeg(2);
    puts("inference success behavior passed");
}

static void verify_mutex_failure(void)
{
    s_create_mutex = false;
    assert(inference_start() == ESP_ERR_NO_MEM);
    assert(s_mutex_create_calls == 1);
    assert(s_allocation_calls == 0);
    assert(s_free_calls == 0);
    assert(s_mutex_delete_calls == 0);
    puts("inference mutex failure passed");
}

static void verify_allocation_failure(int failure_call)
{
    s_fail_allocation_call = failure_call;
    assert(inference_start() == ESP_ERR_NO_MEM);
    assert(s_task_create_calls == 0);
    assert(s_allocation_calls == failure_call);
    assert(s_free_calls == failure_call - 1);
    assert(s_mutex_delete_calls == 1);
    puts("inference allocation failure passed");
}

static void verify_task_failure(void)
{
    s_task_result = pdFAIL;
    assert(inference_start() == ESP_FAIL);
    assert(s_task_create_calls == 1);
    assert(s_free_calls == 4);
    assert(s_mutex_delete_calls == 1);
    s_task_result = pdPASS;
    assert(inference_start() == ESP_OK);
    assert(s_task_create_calls == 2);
    assert(s_allocation_calls == 8);
    puts("inference task failure rollback passed");
}

static void verify_snapshot_unchanged(uint32_t sequence)
{
    assert(get_metadata().sequence == sequence);
    verify_published_jpeg(sequence);
}

static void verify_invalid_frame(void)
{
    verify_start_success();
    verify_first_success();
    s_frame.width = 127;
    assert(inference_run_once() == ESP_ERR_INVALID_ARG);
    verify_snapshot_unchanged(1);
    puts("inference invalid frame cleanup passed");
}

static void verify_oversized_frame(void)
{
    verify_start_success();
    verify_first_success();
    s_frame.len = INFERENCE_MAX_JPEG_BYTES + 1U;
    assert(inference_run_once() == ESP_ERR_INVALID_SIZE);
    verify_snapshot_unchanged(1);
    puts("inference oversized frame preservation passed");
}

static void verify_decode_failure(void)
{
    verify_start_success();
    verify_first_success();
    s_decode_result = false;
    assert(inference_run_once() == ESP_FAIL);
    verify_snapshot_unchanged(1);
    puts("inference decode failure cleanup passed");
}

static void verify_classifier_failure(void)
{
    verify_start_success();
    verify_first_success();
    s_classifier_result = EI_IMPULSE_TFLITE_ERROR;
    assert(inference_run_once() == ESP_FAIL);
    verify_snapshot_unchanged(1);
    puts("inference classifier failure passed");
}

static void verify_resize_failure(void)
{
    verify_start_success();
    verify_first_success();
    const int classifier_calls = s_classifier_calls;
    s_resize_result = -7;
    assert(inference_run_once() == ESP_FAIL);
    assert(s_classifier_calls == classifier_calls);
    verify_snapshot_unchanged(1);
    puts("inference resize failure passed");
}

static void verify_uncertain(void)
{
    verify_start_success();
    s_scores[0] = 0.34f;
    s_scores[1] = 0.33f;
    s_scores[2] = 0.33f;
    assert(inference_run_once() == ESP_OK);
    const inference_snapshot_metadata_t metadata = get_metadata();
    assert(strcmp(metadata.prediction, "uncertain") == 0);
    assert(metadata.confidence == 0.34f);
    assert(s_logs.find("uncertain") != std::string::npos);
    assert(s_logs.find("0.34000") != std::string::npos);
    puts("inference uncertainty reporting passed");
}

int main(int argc, char **argv)
{
    static_assert(INFERENCE_MAX_JPEG_BYTES == CAMERA_MAX_JPEG_BYTES);
    assert(argc == 2);
    if (strcmp(argv[1], "success") == 0) {
        verify_success();
    }
    else if (strcmp(argv[1], "mutex-failure") == 0) {
        verify_mutex_failure();
    }
    else if (strcmp(argv[1], "no-memory-capture-rgb") == 0) {
        verify_allocation_failure(1);
    }
    else if (strcmp(argv[1], "no-memory-model-rgb") == 0) {
        verify_allocation_failure(2);
    }
    else if (strcmp(argv[1], "no-memory-staging") == 0) {
        verify_allocation_failure(3);
    }
    else if (strcmp(argv[1], "no-memory-published") == 0) {
        verify_allocation_failure(4);
    }
    else if (strcmp(argv[1], "task-failure") == 0) {
        verify_task_failure();
    }
    else if (strcmp(argv[1], "invalid-frame") == 0) {
        verify_invalid_frame();
    }
    else if (strcmp(argv[1], "oversized-frame") == 0) {
        verify_oversized_frame();
    }
    else if (strcmp(argv[1], "decode-failure") == 0) {
        verify_decode_failure();
    }
    else if (strcmp(argv[1], "classifier-failure") == 0) {
        verify_classifier_failure();
    }
    else if (strcmp(argv[1], "resize-failure") == 0) {
        verify_resize_failure();
    }
    else if (strcmp(argv[1], "uncertain") == 0) {
        verify_uncertain();
    }
    else {
        assert(false);
    }
    return 0;
}
