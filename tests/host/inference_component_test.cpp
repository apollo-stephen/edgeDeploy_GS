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
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "inference.h"

static uint8_t s_jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
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
static bool s_allocate = true;
static unsigned int s_allocation_caps;
static int s_free_calls;
static BaseType_t s_task_result = pdPASS;
static int s_task_create_calls;
static uint32_t s_task_stack_depth;
static UBaseType_t s_task_priority;
static int s_classifier_calls;
static EI_IMPULSE_ERROR s_classifier_result = EI_IMPULSE_OK;
static float s_scores[EI_CLASSIFIER_LABEL_COUNT] = {0.05f, 0.90f, 0.05f};
static std::vector<uint32_t> s_observed_pixels;
static std::string s_logs;

extern "C" const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test-error";
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
    assert(source_len == sizeof(s_jpeg));
    assert(format == PIXFORMAT_JPEG);
    ++s_decode_calls;
    if (!s_decode_result) {
        return false;
    }

    memset(destination, 0, EI_CLASSIFIER_INPUT_WIDTH *
                               EI_CLASSIFIER_INPUT_HEIGHT * 3U);
    destination[0] = 0x10;
    destination[1] = 0x20;
    destination[2] = 0x30;
    return true;
}

extern "C" void *heap_caps_malloc(size_t size, unsigned int capabilities)
{
    assert(size == EI_CLASSIFIER_INPUT_WIDTH *
                       EI_CLASSIFIER_INPUT_HEIGHT * 3U);
    s_allocation_caps = capabilities;
    return s_allocate ? malloc(size) : nullptr;
}

extern "C" void heap_caps_free(void *pointer)
{
    ++s_free_calls;
    free(pointer);
}

extern "C" BaseType_t xTaskCreate(TaskFunction_t task,
                                   const char *name,
                                   configSTACK_DEPTH_TYPE stack_depth,
                                   void *argument,
                                   UBaseType_t priority,
                                   TaskHandle_t *task_handle)
{
    assert(task != nullptr);
    assert(strcmp(name, "ei_inference") == 0);
    assert(argument == nullptr);
    assert(task_handle == nullptr);
    ++s_task_create_calls;
    s_task_stack_depth = stack_depth;
    s_task_priority = priority;
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

static void verify_start_success(void)
{
    assert(inference_run_once() == ESP_ERR_INVALID_STATE);
    assert(inference_start() == ESP_OK);
    assert(inference_start() == ESP_ERR_INVALID_STATE);
    assert(s_task_create_calls == 1);
    assert(s_task_stack_depth == 8192);
    assert(s_task_priority == 5);
    assert((s_allocation_caps & MALLOC_CAP_SPIRAM) != 0);
    assert((s_allocation_caps & MALLOC_CAP_8BIT) != 0);
}

static void verify_success(void)
{
    verify_start_success();
    assert(inference_run_once() == ESP_OK);
    assert(s_capture_calls == 1);
    assert(s_capture_timeout == 250);
    assert(s_release_calls == 1);
    assert(s_decode_calls == 1);
    assert(s_classifier_calls == 1);
    assert(s_observed_pixels.size() == 2);
    assert(s_observed_pixels[0] == 0x102030);
    assert(s_observed_pixels[1] == 0);
    assert(s_logs.find("recycleable") != std::string::npos);
    assert(s_logs.find("0.90000") != std::string::npos);
    puts("inference success behavior passed");
}

static void verify_no_memory(void)
{
    s_allocate = false;
    assert(inference_start() == ESP_ERR_NO_MEM);
    assert(s_task_create_calls == 0);
    assert(s_free_calls == 0);
    puts("inference allocation failure passed");
}

static void verify_task_failure(void)
{
    s_task_result = pdFAIL;
    assert(inference_start() == ESP_FAIL);
    assert(s_task_create_calls == 1);
    assert(s_free_calls == 1);
    s_task_result = pdPASS;
    assert(inference_start() == ESP_OK);
    assert(s_task_create_calls == 2);
    puts("inference task failure rollback passed");
}

static void verify_invalid_frame(void)
{
    verify_start_success();
    s_frame.width = 127;
    assert(inference_run_once() == ESP_ERR_INVALID_ARG);
    assert(s_release_calls == 1);
    assert(s_decode_calls == 0);
    assert(s_classifier_calls == 0);
    puts("inference invalid frame cleanup passed");
}

static void verify_decode_failure(void)
{
    verify_start_success();
    s_decode_result = false;
    assert(inference_run_once() == ESP_FAIL);
    assert(s_release_calls == 1);
    assert(s_decode_calls == 1);
    assert(s_classifier_calls == 0);
    puts("inference decode failure cleanup passed");
}

static void verify_classifier_failure(void)
{
    verify_start_success();
    s_classifier_result = EI_IMPULSE_TFLITE_ERROR;
    assert(inference_run_once() == ESP_FAIL);
    assert(s_release_calls == 1);
    assert(s_classifier_calls == 1);
    puts("inference classifier failure passed");
}

static void verify_uncertain(void)
{
    verify_start_success();
    s_scores[0] = 0.34f;
    s_scores[1] = 0.33f;
    s_scores[2] = 0.33f;
    assert(inference_run_once() == ESP_OK);
    assert(s_logs.find("uncertain") != std::string::npos);
    assert(s_logs.find("0.34000") != std::string::npos);
    puts("inference uncertainty reporting passed");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "success") == 0) {
        verify_success();
    }
    else if (strcmp(argv[1], "no-memory") == 0) {
        verify_no_memory();
    }
    else if (strcmp(argv[1], "task-failure") == 0) {
        verify_task_failure();
    }
    else if (strcmp(argv[1], "invalid-frame") == 0) {
        verify_invalid_frame();
    }
    else if (strcmp(argv[1], "decode-failure") == 0) {
        verify_decode_failure();
    }
    else if (strcmp(argv[1], "classifier-failure") == 0) {
        verify_classifier_failure();
    }
    else if (strcmp(argv[1], "uncertain") == 0) {
        verify_uncertain();
    }
    else {
        assert(false);
    }
    return 0;
}
