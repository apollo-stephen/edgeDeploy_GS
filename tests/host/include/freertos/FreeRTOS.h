#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef struct {
    int unused;
} portMUX_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMUX_INITIALIZER_UNLOCKED ((portMUX_TYPE){0})
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
