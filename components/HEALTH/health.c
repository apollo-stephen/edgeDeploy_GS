#include "health.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "health_internal.h"

#define HEALTH_SAMPLE_INTERVAL_MS 1000U
#define HEALTH_STARTUP_GRACE_US 7000000ULL
#define HEALTH_INFERENCE_STALE_US 6000000ULL
#define HEALTH_CONSECUTIVE_FAILURE_LIMIT 3U
#define HEALTH_TASK_STACK_BYTES 4096U
#define HEALTH_TASK_PRIORITY 1U

static const char *TAG = "health";
static SemaphoreHandle_t s_snapshot_mutex;
static TaskHandle_t s_task_handle;
static health_snapshot_t s_snapshot;
static uint64_t s_started_us;
static bool s_started;

static health_state_t derive_state(
    uint64_t now_us,
    esp_err_t stats_result,
    const inference_runtime_stats_t *stats,
    uint32_t *reason_flags,
    uint64_t *inference_age_us)
{
    *reason_flags = 0U;
    *inference_age_us = 0U;
    if (stats_result != ESP_OK || stats == NULL) {
        *reason_flags = HEALTH_REASON_STATS_UNAVAILABLE;
        return HEALTH_STATE_DEGRADED;
    }
    if (stats->success_count == 0U) {
        const uint64_t startup_age_us = now_us >= s_started_us
                                            ? now_us - s_started_us
                                            : 0U;
        if (startup_age_us > HEALTH_STARTUP_GRACE_US) {
            *reason_flags |= HEALTH_REASON_STARTUP_TIMEOUT;
            return HEALTH_STATE_DEGRADED;
        }
        return HEALTH_STATE_STARTING;
    }

    *inference_age_us = now_us >= stats->last_success_us
                            ? now_us - stats->last_success_us
                            : 0U;
    if (*inference_age_us > HEALTH_INFERENCE_STALE_US) {
        *reason_flags |= HEALTH_REASON_INFERENCE_STALE;
    }
    if (stats->consecutive_failure_count >=
        HEALTH_CONSECUTIVE_FAILURE_LIMIT) {
        *reason_flags |= HEALTH_REASON_CONSECUTIVE_FAILURES;
    }
    return *reason_flags == 0U ? HEALTH_STATE_HEALTHY
                               : HEALTH_STATE_DEGRADED;
}

static void log_transition(bool had_previous,
                           health_state_t previous,
                           const health_snapshot_t *next)
{
    if ((had_previous && previous == next->state) ||
        (!had_previous && next->state != HEALTH_STATE_DEGRADED)) {
        return;
    }
    if (next->state == HEALTH_STATE_DEGRADED) {
        ESP_LOGW(TAG,
                 "Health degraded: reasons=%" PRIu32
                 " last_error=%d",
                 next->reason_flags,
                 (int)next->inference.last_error);
    }
    else if (next->state == HEALTH_STATE_HEALTHY) {
        ESP_LOGI(TAG, "Health recovered to healthy");
    }
}

esp_err_t health_sample_once(void)
{
    if (!s_started || s_snapshot_mutex == NULL || s_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    inference_runtime_stats_t inference = {0};
    const esp_err_t stats_result =
        inference_get_runtime_stats(&inference);

    health_snapshot_t next = {0};
    next.ready = true;
    next.sampled_us = now_us;
    next.uptime_us = now_us >= s_started_us ? now_us - s_started_us : 0U;
    next.inference = inference;
    next.health_stack_high_water_mark_bytes =
        (uint32_t)uxTaskGetStackHighWaterMark(s_task_handle);

    const unsigned int internal_caps =
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const unsigned int psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    next.internal_free_bytes = heap_caps_get_free_size(internal_caps);
    next.internal_minimum_free_bytes =
        heap_caps_get_minimum_free_size(internal_caps);
    next.internal_largest_free_block_bytes =
        heap_caps_get_largest_free_block(internal_caps);
    next.psram_free_bytes = heap_caps_get_free_size(psram_caps);
    next.psram_minimum_free_bytes =
        heap_caps_get_minimum_free_size(psram_caps);
    next.psram_largest_free_block_bytes =
        heap_caps_get_largest_free_block(psram_caps);
    next.state = derive_state(now_us,
                              stats_result,
                              &inference,
                              &next.reason_flags,
                              &next.inference_age_us);

    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    const bool had_previous = s_snapshot.ready;
    const health_state_t previous = s_snapshot.state;
    next.sequence = s_snapshot.sequence + 1U;
    if (next.sequence == 0U) {
        next.sequence = 1U;
    }
    s_snapshot = next;
    xSemaphoreGive(s_snapshot_mutex);

    log_transition(had_previous, previous, &next);
    return ESP_OK;
}

static void health_task(void *argument)
{
    (void)argument;
    while (true) {
        (void)health_sample_once();
        vTaskDelay(pdMS_TO_TICKS(HEALTH_SAMPLE_INTERVAL_MS));
    }
}

esp_err_t health_start(void)
{
    if (s_started || s_snapshot_mutex != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (s_snapshot_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create health snapshot mutex");
        return ESP_ERR_NO_MEM;
    }

    s_snapshot = (health_snapshot_t){0};
    s_task_handle = NULL;
    s_started_us = (uint64_t)esp_timer_get_time();
    s_started = true;
    const BaseType_t task_result = xTaskCreate(health_task,
                                               "runtime_health",
                                               HEALTH_TASK_STACK_BYTES,
                                               NULL,
                                               HEALTH_TASK_PRIORITY,
                                               &s_task_handle);
    if (task_result != pdPASS) {
        s_started = false;
        s_started_us = 0U;
        s_task_handle = NULL;
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        ESP_LOGE(TAG, "Failed to create health task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t health_get_snapshot(health_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_snapshot_mutex == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (!s_snapshot.ready) {
        xSemaphoreGive(s_snapshot_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = s_snapshot;
    xSemaphoreGive(s_snapshot_mutex);
    return ESP_OK;
}

const char *health_state_name(health_state_t state)
{
    switch (state) {
        case HEALTH_STATE_STARTING: return "starting";
        case HEALTH_STATE_HEALTHY: return "healthy";
        case HEALTH_STATE_DEGRADED: return "degraded";
        default: return NULL;
    }
}
