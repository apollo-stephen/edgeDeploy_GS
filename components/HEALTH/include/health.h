#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "inference.h"

typedef enum {
    HEALTH_STATE_STARTING,
    HEALTH_STATE_HEALTHY,
    HEALTH_STATE_DEGRADED,
} health_state_t;

#define HEALTH_REASON_STARTUP_TIMEOUT (1U << 0)
#define HEALTH_REASON_INFERENCE_STALE (1U << 1)
#define HEALTH_REASON_CONSECUTIVE_FAILURES (1U << 2)
#define HEALTH_REASON_STATS_UNAVAILABLE (1U << 3)

typedef struct {
    bool ready;
    uint32_t sequence;
    health_state_t state;
    uint32_t reason_flags;
    uint64_t sampled_us;
    uint64_t uptime_us;
    uint64_t inference_age_us;
    inference_runtime_stats_t inference;
    uint32_t health_stack_high_water_mark_bytes;
    size_t internal_free_bytes;
    size_t internal_minimum_free_bytes;
    size_t internal_largest_free_block_bytes;
    size_t psram_free_bytes;
    size_t psram_minimum_free_bytes;
    size_t psram_largest_free_block_bytes;
} health_snapshot_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t health_start(void);
esp_err_t health_get_snapshot(health_snapshot_t *snapshot);
const char *health_state_name(health_state_t state);

#ifdef __cplusplus
}
#endif
