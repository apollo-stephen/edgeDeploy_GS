#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
