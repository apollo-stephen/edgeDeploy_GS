#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define INFERENCE_MAX_JPEG_BYTES 8192U
#define INFERENCE_MAX_LABELS 8U
#define INFERENCE_LABEL_BYTES 32U

typedef struct {
    char label[INFERENCE_LABEL_BYTES];
    float value;
} inference_score_t;

typedef struct {
    int dsp_ms;
    int classification_ms;
    int anomaly_ms;
} inference_timing_t;

typedef struct {
    bool ready;
    uint32_t sequence;
    char prediction[INFERENCE_LABEL_BYTES];
    float confidence;
    size_t label_count;
    inference_score_t scores[INFERENCE_MAX_LABELS];
    inference_timing_t timing;
    uint64_t published_ms;
    size_t jpeg_bytes;
} inference_snapshot_metadata_t;

typedef struct {
    bool task_started;
    bool attempt_running;
    uint32_t attempt_count;
    uint32_t success_count;
    uint32_t failure_count;
    uint32_t consecutive_failure_count;
    esp_err_t last_error;
    uint64_t last_attempt_started_us;
    uint64_t last_attempt_finished_us;
    uint64_t last_success_us;
    uint64_t last_duration_us;
    uint64_t max_duration_us;
    uint32_t stack_high_water_mark_bytes;
} inference_runtime_stats_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t inference_start(void);
esp_err_t inference_run_once(void);
esp_err_t inference_get_latest_metadata(
    inference_snapshot_metadata_t *metadata);
esp_err_t inference_get_runtime_stats(inference_runtime_stats_t *stats);
esp_err_t inference_copy_latest_jpeg(uint32_t expected_sequence,
                                     uint8_t *destination,
                                     size_t capacity,
                                     size_t *jpeg_bytes);

#ifdef __cplusplus
}
#endif
