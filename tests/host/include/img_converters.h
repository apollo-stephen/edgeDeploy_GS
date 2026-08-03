#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

bool fmt2rgb888(const uint8_t *source,
                size_t source_len,
                pixformat_t format,
                uint8_t *destination);

#ifdef __cplusplus
}
#endif
