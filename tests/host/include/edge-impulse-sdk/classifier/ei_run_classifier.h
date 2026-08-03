#pragma once

#include <stddef.h>

#include <functional>

#define EI_CLASSIFIER_INPUT_WIDTH 128
#define EI_CLASSIFIER_INPUT_HEIGHT 128
#define EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE 16384
#define EI_CLASSIFIER_LABEL_COUNT 3
#define EI_CLASSIFIER_THRESHOLD 0.6f

typedef enum {
    EI_IMPULSE_OK = 0,
    EI_IMPULSE_TFLITE_ERROR = -5,
} EI_IMPULSE_ERROR;

namespace ei {
typedef struct ei_signal_t {
    std::function<int(size_t offset, size_t length, float *out_ptr)> get_data;
    size_t total_length;
} signal_t;
}

typedef struct {
    const char *label;
    float value;
} ei_impulse_result_classification_t;

typedef struct {
    int dsp;
    int classification;
    int postprocessing;
    int anomaly;
} ei_impulse_result_timing_t;

typedef struct {
    ei_impulse_result_classification_t
        classification[EI_CLASSIFIER_LABEL_COUNT];
    float anomaly;
    ei_impulse_result_timing_t timing;
} ei_impulse_result_t;

inline constexpr const char *ei_classifier_inferencing_categories[] = {
    "harmful",
    "recycleable",
    "wet",
};

EI_IMPULSE_ERROR run_classifier(ei::signal_t *signal,
                                ei_impulse_result_t *result,
                                bool debug);
