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
#define HEALTH_CLIENT_LEASE_US 10000000ULL
#define HEALTH_STOP_TIMEOUT_MS 2000U
#define HEALTH_STOP_POLL_MS 10U

static const char *TAG = "health";
static SemaphoreHandle_t s_snapshot_mutex;
static TaskHandle_t s_task_handle;
static health_snapshot_t s_snapshot;
static uint64_t s_started_us;
static bool s_started;
static portMUX_TYPE s_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static health_monitor_lifecycle_t s_lifecycle = HEALTH_MONITOR_OFF;
static bool s_stop_requested;
static uint64_t s_lease_refreshed_us;

static health_monitor_lifecycle_t get_lifecycle(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    const health_monitor_lifecycle_t lifecycle = s_lifecycle;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return lifecycle;
}

bool health_test_lease_expired(uint64_t now_us)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    const bool enabled = s_lifecycle == HEALTH_MONITOR_STARTING ||
                         s_lifecycle == HEALTH_MONITOR_RUNNING;
    const uint64_t refreshed_us = s_lease_refreshed_us;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return enabled && now_us >= refreshed_us &&
           now_us - refreshed_us >= HEALTH_CLIENT_LEASE_US;
}

static bool health_task_should_exit(uint64_t now_us)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    const bool stop_requested = s_stop_requested;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return stop_requested || health_test_lease_expired(now_us);
}

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
    while (!health_task_should_exit((uint64_t)esp_timer_get_time())) {
        (void)health_sample_once();
        (void)ulTaskNotifyTake(pdTRUE,
                               pdMS_TO_TICKS(HEALTH_SAMPLE_INTERVAL_MS));
    }

    if (s_snapshot_mutex != NULL &&
        xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        s_snapshot = (health_snapshot_t){0};
        xSemaphoreGive(s_snapshot_mutex);
    }
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_started = false;
    s_started_us = 0U;
    s_task_handle = NULL;
    s_stop_requested = false;
    s_lifecycle = HEALTH_MONITOR_OFF;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    vTaskDelete(NULL);
}

static void rollback_start(bool delete_mutex)
{
    if (delete_mutex && s_snapshot_mutex != NULL) {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
    }
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_started = false;
    s_started_us = 0U;
    s_task_handle = NULL;
    s_stop_requested = false;
    s_lease_refreshed_us = 0U;
    s_lifecycle = HEALTH_MONITOR_OFF;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}

static esp_err_t start_monitor(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_lifecycle_lock);
    if (s_lifecycle == HEALTH_MONITOR_RUNNING ||
        s_lifecycle == HEALTH_MONITOR_STARTING) {
        s_lease_refreshed_us = now_us;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_OK;
    }
    if (s_lifecycle == HEALTH_MONITOR_STOPPING) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_lifecycle = HEALTH_MONITOR_STARTING;
    s_stop_requested = false;
    s_lease_refreshed_us = now_us;
    portEXIT_CRITICAL(&s_lifecycle_lock);

    bool created_mutex = false;
    if (s_snapshot_mutex == NULL) {
        s_snapshot_mutex = xSemaphoreCreateMutex();
        if (s_snapshot_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create health snapshot mutex");
            rollback_start(false);
            return ESP_ERR_NO_MEM;
        }
        created_mutex = true;
    }

    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        rollback_start(created_mutex);
        return ESP_FAIL;
    }
    s_snapshot = (health_snapshot_t){0};
    xSemaphoreGive(s_snapshot_mutex);
    s_task_handle = NULL;
    s_started_us = now_us;
    s_started = true;
    const BaseType_t task_result = xTaskCreate(health_task,
                                               "runtime_health",
                                               HEALTH_TASK_STACK_BYTES,
                                               NULL,
                                               HEALTH_TASK_PRIORITY,
                                               &s_task_handle);
    if (task_result != pdPASS) {
        rollback_start(created_mutex);
        ESP_LOGE(TAG, "Failed to create health task");
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_lifecycle = HEALTH_MONITOR_RUNNING;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return ESP_OK;
}

static esp_err_t stop_monitor(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    if (s_lifecycle == HEALTH_MONITOR_OFF) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_OK;
    }
    s_lifecycle = HEALTH_MONITOR_STOPPING;
    s_stop_requested = true;
    const TaskHandle_t task = s_task_handle;
    portEXIT_CRITICAL(&s_lifecycle_lock);

    if (task == NULL) {
        rollback_start(false);
        return ESP_OK;
    }
    (void)xTaskNotifyGive(task);
    const uint32_t polls = HEALTH_STOP_TIMEOUT_MS / HEALTH_STOP_POLL_MS;
    for (uint32_t index = 0; index < polls; ++index) {
        if (get_lifecycle() == HEALTH_MONITOR_OFF) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(HEALTH_STOP_POLL_MS));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t health_set_enabled(bool enabled)
{
    return enabled ? start_monitor() : stop_monitor();
}

esp_err_t health_refresh_lease(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_lifecycle_lock);
    if (s_lifecycle != HEALTH_MONITOR_RUNNING &&
        s_lifecycle != HEALTH_MONITOR_STARTING) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_lease_refreshed_us = now_us;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return ESP_OK;
}

esp_err_t health_get_monitor_status(health_monitor_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lifecycle_lock);
    status->lifecycle = s_lifecycle;
    status->enabled = s_lifecycle != HEALTH_MONITOR_OFF;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    status->ready = false;
    if (s_snapshot_mutex != NULL &&
        xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
        status->ready = s_snapshot.ready;
        xSemaphoreGive(s_snapshot_mutex);
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
