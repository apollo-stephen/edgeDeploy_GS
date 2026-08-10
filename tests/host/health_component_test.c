#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "health.h"
#include "health_internal.h"

struct fake_semaphore {
    int unused;
};

static struct fake_semaphore s_mutex;
static bool s_create_mutex = true;
static int s_mutex_create_calls;
static int s_mutex_take_calls;
static int s_mutex_give_calls;
static int s_mutex_delete_calls;
static BaseType_t s_task_result = pdPASS;
static int s_task_create_calls;
static TaskFunction_t s_task_function;
static int s_fake_task_storage;
static TaskHandle_t s_task_handle = &s_fake_task_storage;
static configSTACK_DEPTH_TYPE s_task_stack_depth;
static UBaseType_t s_task_priority;
static UBaseType_t s_stack_high_water_mark = 1536;
static int64_t s_now_us;
static esp_err_t s_inference_result = ESP_OK;
static inference_runtime_stats_t s_inference_stats;
static size_t s_internal_free = 123456;
static size_t s_internal_minimum = 120000;
static size_t s_internal_largest = 65536;
static size_t s_psram_free = 654321;
static size_t s_psram_minimum = 640000;
static size_t s_psram_largest = 524288;
static int s_warning_logs;
static int s_info_logs;
static char s_last_log[256];

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test-error";
}

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

esp_err_t inference_get_runtime_stats(inference_runtime_stats_t *stats)
{
    assert(stats != NULL);
    if (s_inference_result != ESP_OK) {
        return s_inference_result;
    }
    *stats = s_inference_stats;
    return ESP_OK;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    ++s_mutex_create_calls;
    return s_create_mutex ? &s_mutex : NULL;
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

BaseType_t xTaskCreate(TaskFunction_t task,
                       const char *name,
                       configSTACK_DEPTH_TYPE stack_depth,
                       void *argument,
                       UBaseType_t priority,
                       TaskHandle_t *task_handle)
{
    assert(task != NULL);
    assert(strcmp(name, "runtime_health") == 0);
    assert(argument == NULL);
    assert(task_handle != NULL);
    ++s_task_create_calls;
    s_task_function = task;
    s_task_stack_depth = stack_depth;
    s_task_priority = priority;
    if (s_task_result == pdPASS) {
        *task_handle = s_task_handle;
    }
    return s_task_result;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    assert(task == s_task_handle);
    return s_stack_high_water_mark;
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks == pdMS_TO_TICKS(1000));
}

static bool is_internal_caps(unsigned int capabilities)
{
    return capabilities == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static bool is_psram_caps(unsigned int capabilities)
{
    return capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

size_t heap_caps_get_free_size(unsigned int capabilities)
{
    assert(is_internal_caps(capabilities) || is_psram_caps(capabilities));
    return is_internal_caps(capabilities) ? s_internal_free : s_psram_free;
}

size_t heap_caps_get_minimum_free_size(unsigned int capabilities)
{
    assert(is_internal_caps(capabilities) || is_psram_caps(capabilities));
    return is_internal_caps(capabilities) ? s_internal_minimum
                                          : s_psram_minimum;
}

size_t heap_caps_get_largest_free_block(unsigned int capabilities)
{
    assert(is_internal_caps(capabilities) || is_psram_caps(capabilities));
    return is_internal_caps(capabilities) ? s_internal_largest
                                          : s_psram_largest;
}

void test_log_write(const char *level,
                    const char *tag,
                    const char *format,
                    ...)
{
    assert(strcmp(tag, "health") == 0);
    if (strcmp(level, "W") == 0) {
        ++s_warning_logs;
    }
    else if (strcmp(level, "I") == 0) {
        ++s_info_logs;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(s_last_log, sizeof(s_last_log), format, args);
    va_end(args);
}

static health_snapshot_t get_snapshot(void)
{
    health_snapshot_t snapshot = {0};
    assert(health_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.ready);
    return snapshot;
}

static void assert_snapshot_state(health_state_t state, uint32_t reasons)
{
    const health_snapshot_t snapshot = get_snapshot();
    assert(snapshot.state == state);
    assert(snapshot.reason_flags == reasons);
}

static void start_successfully(void)
{
    health_snapshot_t snapshot = {0};
    assert(health_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    assert(health_get_snapshot(&snapshot) == ESP_ERR_NOT_FOUND);
    assert(health_start() == ESP_OK);
    assert(health_start() == ESP_ERR_INVALID_STATE);
    assert(s_mutex_create_calls == 1);
    assert(s_task_create_calls == 1);
    assert(s_task_function != NULL);
    assert(s_task_stack_depth == 4096);
    assert(s_task_priority == 1);
    assert(health_get_snapshot(&snapshot) == ESP_ERR_NOT_FOUND);
}

static void verify_lifecycle(void)
{
    s_create_mutex = false;
    assert(health_start() == ESP_ERR_NO_MEM);
    assert(s_task_create_calls == 0);
    assert(s_mutex_delete_calls == 0);

    s_create_mutex = true;
    s_task_result = pdFAIL;
    assert(health_start() == ESP_FAIL);
    assert(s_task_create_calls == 1);
    assert(s_mutex_delete_calls == 1);

    s_task_result = pdPASS;
    assert(health_start() == ESP_OK);
    assert(health_start() == ESP_ERR_INVALID_STATE);
    assert(s_task_create_calls == 2);
    assert(strcmp(health_state_name(HEALTH_STATE_STARTING), "starting") == 0);
    assert(strcmp(health_state_name(HEALTH_STATE_HEALTHY), "healthy") == 0);
    assert(strcmp(health_state_name(HEALTH_STATE_DEGRADED), "degraded") == 0);
    assert(health_state_name((health_state_t)99) == NULL);
    puts("health lifecycle behavior passed");
}

static void verify_transitions(void)
{
    start_successfully();

    s_now_us = 0;
    assert(health_sample_once() == ESP_OK);
    assert_snapshot_state(HEALTH_STATE_STARTING, 0U);
    assert(s_warning_logs == 0);
    assert(s_info_logs == 0);

    s_now_us = 7000001;
    assert(health_sample_once() == ESP_OK);
    assert_snapshot_state(HEALTH_STATE_DEGRADED,
                          HEALTH_REASON_STARTUP_TIMEOUT);
    assert(s_warning_logs == 1);
    assert(strstr(s_last_log, "degraded") != NULL);

    assert(health_sample_once() == ESP_OK);
    assert(s_warning_logs == 1);

    s_inference_stats.success_count = 1;
    s_inference_stats.last_success_us = s_now_us;
    assert(health_sample_once() == ESP_OK);
    assert_snapshot_state(HEALTH_STATE_HEALTHY, 0U);
    assert(s_info_logs == 1);

    s_now_us += 6000001;
    assert(health_sample_once() == ESP_OK);
    assert_snapshot_state(HEALTH_STATE_DEGRADED,
                          HEALTH_REASON_INFERENCE_STALE);
    assert(s_warning_logs == 2);

    s_inference_stats.consecutive_failure_count = 3;
    assert(health_sample_once() == ESP_OK);
    assert_snapshot_state(
        HEALTH_STATE_DEGRADED,
        HEALTH_REASON_INFERENCE_STALE |
            HEALTH_REASON_CONSECUTIVE_FAILURES);
    assert(s_warning_logs == 2);

    s_inference_stats.consecutive_failure_count = 0;
    s_inference_stats.last_success_us = s_now_us;
    assert(health_sample_once() == ESP_OK);
    assert_snapshot_state(HEALTH_STATE_HEALTHY, 0U);
    assert(s_info_logs == 2);
    puts("health transition behavior passed");
}

static void verify_resources(void)
{
    s_now_us = 1000000;
    start_successfully();
    s_now_us = 2500000;
    s_inference_stats.task_started = true;
    s_inference_stats.success_count = 2;
    s_inference_stats.attempt_count = 3;
    s_inference_stats.failure_count = 1;
    s_inference_stats.last_success_us = 2250000;
    s_inference_stats.last_duration_us = 132000;
    s_inference_stats.max_duration_us = 149000;
    s_inference_stats.stack_high_water_mark_bytes = 3072;

    assert(health_sample_once() == ESP_OK);
    const health_snapshot_t snapshot = get_snapshot();
    assert(snapshot.sequence == 1);
    assert(snapshot.state == HEALTH_STATE_HEALTHY);
    assert(snapshot.sampled_us == 2500000);
    assert(snapshot.uptime_us == 1500000);
    assert(snapshot.inference_age_us == 250000);
    assert(snapshot.inference.attempt_count == 3);
    assert(snapshot.inference.success_count == 2);
    assert(snapshot.inference.failure_count == 1);
    assert(snapshot.inference.last_duration_us == 132000);
    assert(snapshot.inference.max_duration_us == 149000);
    assert(snapshot.health_stack_high_water_mark_bytes == 1536);
    assert(snapshot.internal_free_bytes == 123456);
    assert(snapshot.internal_minimum_free_bytes == 120000);
    assert(snapshot.internal_largest_free_block_bytes == 65536);
    assert(snapshot.psram_free_bytes == 654321);
    assert(snapshot.psram_minimum_free_bytes == 640000);
    assert(snapshot.psram_largest_free_block_bytes == 524288);
    assert(s_mutex_take_calls == s_mutex_give_calls);
    puts("health resource snapshot behavior passed");
}

static void verify_stats_unavailable(void)
{
    start_successfully();
    s_now_us = 1000;
    s_inference_result = ESP_FAIL;
    assert(health_sample_once() == ESP_OK);
    assert_snapshot_state(HEALTH_STATE_DEGRADED,
                          HEALTH_REASON_STATS_UNAVAILABLE);
    assert(s_warning_logs == 1);
    puts("health unavailable statistics behavior passed");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "lifecycle") == 0) {
        verify_lifecycle();
    }
    else if (strcmp(argv[1], "transitions") == 0) {
        verify_transitions();
    }
    else if (strcmp(argv[1], "resources") == 0) {
        verify_resources();
    }
    else if (strcmp(argv[1], "stats-unavailable") == 0) {
        verify_stats_unavailable();
    }
    else {
        assert(!"unknown scenario");
    }
    return 0;
}
